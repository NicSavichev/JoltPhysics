// SPDX-FileCopyrightText: 2021 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#include <Jolt.h>

#include <unordered_map>
#include <Physics/Collision/Shape/MeshShape.h>
#include <Physics/Collision/Shape/ConvexShape.h>
#include <Physics/Collision/Shape/ScaleHelpers.h>
#include <Physics/Collision/RayCast.h>
#include <Physics/Collision/ShapeCast.h>
#include <Physics/Collision/ShapeFilter.h>
#include <Physics/Collision/CastResult.h>
#include <Physics/Collision/CollidePointResult.h>
#include <Physics/Collision/CollideConvexVsTriangles.h>
#include <Physics/Collision/CastConvexVsTriangles.h>
#include <Physics/Collision/TransformedShape.h>
#include <Physics/Collision/ActiveEdges.h>
#include <Physics/Collision/CollisionDispatch.h>
#include <Core/StringTools.h>
#include <Core/StreamIn.h>
#include <Core/StreamOut.h>
#include <Core/Profiler.h>
#include <Geometry/AABox4.h>
#include <Geometry/RayAABox.h>
#include <Geometry/Indexify.h>
#include <Geometry/Plane.h>
#include <Geometry/OrientedBox.h>
#include <TriangleSplitter/TriangleSplitterBinning.h>
#include <AABBTree/AABBTreeBuilder.h>
#include <AABBTree/AABBTreeToBuffer.h>
#include <AABBTree/TriangleCodec/TriangleCodecIndexed8BitPackSOA4Flags.h>
#include <AABBTree/NodeCodec/NodeCodecQuadTreeHalfFloat.h>
#include <ObjectStream/TypeDeclarations.h>

namespace JPH {

#ifdef JPH_DEBUG_RENDERER
bool MeshShape::sDrawTriangleGroups = false;
bool MeshShape::sDrawTriangleOutlines = false;
#endif // JPH_DEBUG_RENDERER

JPH_IMPLEMENT_SERIALIZABLE_VIRTUAL(MeshShapeSettings)
{
	JPH_ADD_BASE_CLASS(MeshShapeSettings, ShapeSettings)

	JPH_ADD_ATTRIBUTE(MeshShapeSettings, mTriangleVertices)
	JPH_ADD_ATTRIBUTE(MeshShapeSettings, mIndexedTriangles)
	JPH_ADD_ATTRIBUTE(MeshShapeSettings, mMaterials)
}

// Codecs this mesh shape is using
using TriangleCodec = TriangleCodecIndexed8BitPackSOA4Flags;
using NodeCodec = NodeCodecQuadTreeHalfFloat<1>;

// Get header for tree
static inline const NodeCodec::Header *sGetNodeHeader(const ByteBuffer &inTree)
{
	return inTree.Get<NodeCodec::Header>(0);
}

// Get header for triangles
static inline const TriangleCodec::TriangleHeader *sGetTriangleHeader(const ByteBuffer &inTree) 
{
	return inTree.Get<TriangleCodec::TriangleHeader>(NodeCodec::HeaderSize);
}

MeshShapeSettings::MeshShapeSettings(const TriangleList &inTriangles, const PhysicsMaterialList &inMaterials) :
	mMaterials(inMaterials)
{
	Indexify(inTriangles, mTriangleVertices, mIndexedTriangles);

	Sanitize();
}

MeshShapeSettings::MeshShapeSettings(const VertexList &inVertices, const IndexedTriangleList &inTriangles, const PhysicsMaterialList &inMaterials) :
	mTriangleVertices(inVertices),
	mIndexedTriangles(inTriangles),
	mMaterials(inMaterials)
{
	Sanitize();
}

void MeshShapeSettings::Sanitize()
{
	// Remove degenerate and duplicate triangles
	unordered_set<IndexedTriangle> triangles;
	triangles.reserve(mIndexedTriangles.size());
	for (int t = (int)mIndexedTriangles.size() - 1; t >= 0; --t)
	{
		const IndexedTriangle &tri = mIndexedTriangles[t];
		if (tri.IsDegenerate()										// Degenerate triangle
			|| !triangles.insert(tri.GetLowestIndexFirst()).second) // Duplicate triangle
			mIndexedTriangles.erase(mIndexedTriangles.begin() + t);
	}
}

ShapeSettings::ShapeResult MeshShapeSettings::Create() const
{
	if (mCachedResult.IsEmpty())
		Ref<Shape> shape = new MeshShape(*this, mCachedResult); 
	return mCachedResult;
}

MeshShape::MeshShape(const MeshShapeSettings &inSettings, ShapeResult &outResult) : 
	Shape(EShapeType::Mesh, EShapeSubType::Mesh, inSettings, outResult)
{
	// Check if there are any triangles
	if (inSettings.mIndexedTriangles.empty())
	{
		outResult.SetError("Need triangles to create a mesh shape!");
		return;
	}

	// Check triangles
	for (int t = (int)inSettings.mIndexedTriangles.size() - 1; t >= 0; --t)
	{
		const IndexedTriangle &triangle = inSettings.mIndexedTriangles[t];
		if (triangle.IsDegenerate())
		{
			outResult.SetError(StringFormat("Triangle %d is degenerate!", t));
			return;
		}
		else
		{
			// Check vertex indices
			for (int i = 0; i < 3; ++i)
				if (triangle.mIdx[i] >= inSettings.mTriangleVertices.size())
				{
					outResult.SetError(StringFormat("Vertex index %u is beyond vertex list (size: %u)", triangle.mIdx[i], (uint)inSettings.mTriangleVertices.size()));
					return;
				}
		}
	}

	// Copy materials
	mMaterials = inSettings.mMaterials;
	if (!mMaterials.empty())
	{
		// Validate materials
		if (mMaterials.size() > FLAGS_MATERIAL_MASK)
		{
			outResult.SetError(StringFormat("Supporting max %d materials per mesh", FLAGS_ACTIVE_EDGE_MASK + 1));
			return;
		}
		for (const IndexedTriangle &t : inSettings.mIndexedTriangles)
			if (t.mMaterialIndex >= mMaterials.size())
			{
				outResult.SetError(StringFormat("Triangle material %u is beyond material list (size: %u)", t.mMaterialIndex, (uint)mMaterials.size()));
				return;
			}
	}
	else
	{
		// No materials assigned, validate that all triangles use material index 0
		for (const IndexedTriangle &t : inSettings.mIndexedTriangles)
			if (t.mMaterialIndex != 0)
			{
				outResult.SetError("No materials present, all triangles should have material index 0");
				return;
			}
	}

	// Fill in active edge bits
	IndexedTriangleList indexed_triangles = inSettings.mIndexedTriangles; // Copy indices since we're adding the 'active edge' flag
	FindActiveEdges(inSettings.mTriangleVertices, indexed_triangles);

	// Create triangle splitter
	TriangleSplitterBinning splitter(inSettings.mTriangleVertices, indexed_triangles);
	
	// Build tree
	AABBTreeBuilder builder(splitter, MaxTrianglesPerLeaf);
	AABBTreeBuilderStats builder_stats;
	AABBTreeBuilder::Node *root = builder.Build(builder_stats);

	// Convert to buffer
	AABBTreeToBufferStats buffer_stats;
	AABBTreeToBuffer<TriangleCodec, NodeCodec> buffer;
	string error;
	if (!buffer.Convert(inSettings.mTriangleVertices, root, buffer_stats, error, EAABBTreeToBufferConvertMode::DepthFirstTrianglesLast))
	{
		outResult.SetError(move(error));
		delete root;
		return;
	}

	// Kill tree
	delete root;

	// Move data to this class
	mTree.swap(buffer.GetBuffer());

	// Check if we're not exceeding the amount of sub shape id bits
	if (GetSubShapeIDBitsRecursive() > SubShapeID::MaxBits)
	{
		outResult.SetError("Mesh is too big and exceeds the amount of available sub shape ID bits");
		return;
	}

	outResult.Set(this);
}

void MeshShape::FindActiveEdges(const VertexList &inVertices, IndexedTriangleList &ioIndices)
{
	struct Edge
	{
				Edge(int inIdx1, int inIdx2) : mIdx1(min(inIdx1, inIdx2)), mIdx2(max(inIdx1, inIdx2)) { }

		uint	GetIndexInTriangle(const IndexedTriangle &inTriangle) const
		{
			for (uint edge_idx = 0; edge_idx < 3; ++edge_idx)
			{
				Edge edge(inTriangle.mIdx[edge_idx], inTriangle.mIdx[(edge_idx + 1) % 3]);
				if (*this == edge)
					return edge_idx;
			}

			JPH_ASSERT(false);
			return ~uint(0);
		}

		bool	operator == (const Edge &inRHS) const
		{
			return mIdx1 == inRHS.mIdx1 && mIdx2 == inRHS.mIdx2;
		}

		int		mIdx1;
		int		mIdx2;
	};

	JPH_MAKE_HASH_STRUCT(Edge, EdgeHash, t.mIdx1, t.mIdx2)

	// Build a list of edge to triangles
	using EdgeToTriangle = unordered_map<Edge, vector<uint>, EdgeHash>;
	EdgeToTriangle edge_to_triangle;
	for (uint triangle_idx = 0; triangle_idx < ioIndices.size(); ++triangle_idx)
	{
		const IndexedTriangle &triangle = ioIndices[triangle_idx];
		for (uint edge_idx = 0; edge_idx < 3; ++edge_idx)
		{
			Edge edge(triangle.mIdx[edge_idx], triangle.mIdx[(edge_idx + 1) % 3]);
			edge_to_triangle[edge].push_back(triangle_idx);
		}
	}

	// Walk over all edges and determine which ones are active
	for (const EdgeToTriangle::value_type &edge : edge_to_triangle)
	{
		bool active = false;
		if (edge.second.size() == 1)
		{
			// Edge is not shared, it is an active edge
			active = true;
		}
		else if (edge.second.size() == 2)
		{
			// Simple shared edge, determine if edge is active based on the two adjacent triangles
			const IndexedTriangle &triangle1 = ioIndices[edge.second[0]];
			const IndexedTriangle &triangle2 = ioIndices[edge.second[1]];

			// Find which edge this is for both triangles
			uint edge_idx1 = edge.first.GetIndexInTriangle(triangle1);
			uint edge_idx2 = edge.first.GetIndexInTriangle(triangle2);

			// Construct a plane for triangle 1 (e1 = edge vertex 1, e2 = edge vertex 2, op = opposing vertex)
			Vec3 triangle1_e1 = Vec3(inVertices[triangle1.mIdx[edge_idx1]]);
			Vec3 triangle1_e2 = Vec3(inVertices[triangle1.mIdx[(edge_idx1 + 1) % 3]]);
			Vec3 triangle1_op = Vec3(inVertices[triangle1.mIdx[(edge_idx1 + 2) % 3]]);
			Plane triangle1_plane = Plane::sFromPointsCCW(triangle1_e1, triangle1_e2, triangle1_op);

			// Construct a plane for triangle 2
			Vec3 triangle2_e1 = Vec3(inVertices[triangle2.mIdx[edge_idx2]]);
			Vec3 triangle2_e2 = Vec3(inVertices[triangle2.mIdx[(edge_idx2 + 1) % 3]]);
			Vec3 triangle2_op = Vec3(inVertices[triangle2.mIdx[(edge_idx2 + 2) % 3]]);
			Plane triangle2_plane = Plane::sFromPointsCCW(triangle2_e1, triangle2_e2, triangle2_op);

			// Determine if the edge is active
			active = ActiveEdges::IsEdgeActive(triangle1_plane.GetNormal(), triangle2_plane.GetNormal(), triangle1_e2 - triangle1_e1);
		}
		else
		{
			// Multiple edges incoming, assume active
			active = true;
		}

		if (active)
		{
			// Mark edges of all original triangles active
			for (uint triangle_idx : edge.second)
			{
				IndexedTriangle &triangle = ioIndices[triangle_idx];
				uint edge_idx = edge.first.GetIndexInTriangle(triangle);
				uint32 mask = 1 << (edge_idx + FLAGS_ACTIVE_EGDE_SHIFT);
				JPH_ASSERT((triangle.mMaterialIndex & mask) == 0);
				triangle.mMaterialIndex |= mask;
			}
		}
	}
}

MassProperties MeshShape::GetMassProperties() const
{
	// Object should always be static, return default mass properties
	return MassProperties();
}

void MeshShape::DecodeSubShapeID(const SubShapeID &inSubShapeID, const void *&outTriangleBlock, uint32 &outTriangleIndex) const
{
	// Get block
	SubShapeID triangle_idx_subshape_id;	
	uint32 block_id = inSubShapeID.PopID(NodeCodec::DecodingContext::sTriangleBlockIDBits(mTree), triangle_idx_subshape_id);
	outTriangleBlock = NodeCodec::DecodingContext::sGetTriangleBlockStart(&mTree[0], block_id);

	// Fetch the triangle index
	SubShapeID remainder;
	outTriangleIndex = triangle_idx_subshape_id.PopID(NumTriangleBits, remainder);
	JPH_ASSERT(remainder.IsEmpty(), "Invalid subshape ID");
}

const PhysicsMaterial *MeshShape::GetMaterial(const SubShapeID &inSubShapeID) const
{
	// Return the default material if there are no materials on this shape
	if (mMaterials.empty())
		return PhysicsMaterial::sDefault;

	// Decode ID
	const void *block_start;
	uint32 triangle_idx;
	DecodeSubShapeID(inSubShapeID, block_start, triangle_idx);
		
	// Fetch the flags
	uint8 flags = TriangleCodec::DecodingContext::sGetFlags(block_start, triangle_idx);
	return mMaterials[flags & FLAGS_MATERIAL_MASK];
}

Vec3 MeshShape::GetSurfaceNormal(const SubShapeID &inSubShapeID, Vec3Arg inLocalSurfacePosition) const 
{ 
	// Decode ID
	const void *block_start;
	uint32 triangle_idx;
	DecodeSubShapeID(inSubShapeID, block_start, triangle_idx);

	// Decode triangle
	Vec3 v1, v2, v3;
	const TriangleCodec::DecodingContext triangle_ctx(sGetTriangleHeader(mTree), mTree);
	triangle_ctx.GetTriangle(block_start, triangle_idx, v1, v2, v3);

	//  Calculate normal
	return (v3 - v2).Cross(v1 - v2).Normalized();
}

AABox MeshShape::GetLocalBounds() const
{
	const NodeCodec::Header *header = sGetNodeHeader(mTree);
	return AABox(Vec3::sLoadFloat3Unsafe(header->mRootBoundsMin), Vec3::sLoadFloat3Unsafe(header->mRootBoundsMax));
}

uint MeshShape::GetSubShapeIDBitsRecursive() const 
{
	return NodeCodec::DecodingContext::sTriangleBlockIDBits(mTree) + NumTriangleBits;
}

template <class Visitor>
void MeshShape::WalkTree(Visitor &ioVisitor) const
{
	const NodeCodec::Header *header = sGetNodeHeader(mTree);
	NodeCodec::DecodingContext node_ctx(header);

	const TriangleCodec::DecodingContext triangle_ctx(sGetTriangleHeader(mTree), mTree);
	const uint8 *buffer_start = &mTree[0];
	node_ctx.WalkTree(buffer_start, triangle_ctx, ioVisitor);
}

#ifdef JPH_DEBUG_RENDERER
void MeshShape::Draw(DebugRenderer *inRenderer, Mat44Arg inCenterOfMassTransform, Vec3Arg inScale, ColorArg inColor, bool inUseMaterialColors, bool inDrawWireframe) const
{
	// Reset the batch if we switch coloring mode
	if (mCachedTrianglesColoredPerGroup != sDrawTriangleGroups || mCachedUseMaterialColors != inUseMaterialColors)
	{
		mGeometry = nullptr;
		mCachedTrianglesColoredPerGroup = sDrawTriangleGroups;
		mCachedUseMaterialColors = inUseMaterialColors;
	}

	if (mGeometry == nullptr)
	{
		struct Visitor
		{
			bool	ShouldAbort() const
			{
				return false;
			}

			bool	ShouldVisitNode(int inStackTop) const
			{
				return true;
			}

			int		VisitNodes(Vec4Arg inBoundsMinX, Vec4Arg inBoundsMinY, Vec4Arg inBoundsMinZ, Vec4Arg inBoundsMaxX, Vec4Arg inBoundsMaxY, Vec4Arg inBoundsMaxZ, UVec4 &ioProperties, int inStackTop) 
			{
				UVec4 valid = UVec4::sOr(UVec4::sOr(Vec4::sLess(inBoundsMinX, inBoundsMaxX), Vec4::sLess(inBoundsMinY, inBoundsMaxY)), Vec4::sLess(inBoundsMinZ, inBoundsMaxZ));
				UVec4::sSort4True(valid, ioProperties);
				return valid.CountTrues();
			}

			void	VisitTriangles(const TriangleCodec::DecodingContext &ioContext, Vec3Arg inRootBoundsMin, Vec3Arg inRootBoundsMax, const void *inTriangles, int inNumTriangles, uint32 inTriangleBlockID) 
			{
				JPH_ASSERT(inNumTriangles <= MaxTrianglesPerLeaf);
				Vec3 vertices[MaxTrianglesPerLeaf * 3];
				ioContext.Unpack(inRootBoundsMin, inRootBoundsMax, inTriangles, inNumTriangles, vertices);

				if (mDrawTriangleGroups || !mUseMaterialColors || mMaterials.empty())
				{
					// Single color for mesh
					Color color = mDrawTriangleGroups? Color::sGetDistinctColor(mColorIdx++) : (mUseMaterialColors? PhysicsMaterial::sDefault->GetDebugColor() : Color::sWhite);
					for (const Vec3 *v = vertices, *v_end = vertices + inNumTriangles * 3; v < v_end; v += 3)
						mTriangles.push_back({ v[0], v[1], v[2], color });
				}
				else
				{
					// Per triangle color
					uint8 flags[MaxTrianglesPerLeaf];
					TriangleCodec::DecodingContext::sGetFlags(inTriangles, inNumTriangles, flags);

					const uint8 *f = flags;
					for (const Vec3 *v = vertices, *v_end = vertices + inNumTriangles * 3; v < v_end; v += 3, f++)
						mTriangles.push_back({ v[0], v[1], v[2], mMaterials[*f & FLAGS_MATERIAL_MASK]->GetDebugColor() });
				}
			}

			vector<DebugRenderer::Triangle> &		mTriangles;
			const PhysicsMaterialList &				mMaterials;
			bool									mUseMaterialColors;
			bool									mDrawTriangleGroups;
			int										mColorIdx = 0;
		};
		
		vector<DebugRenderer::Triangle> triangles;
		Visitor visitor { triangles, mMaterials, mCachedUseMaterialColors, mCachedTrianglesColoredPerGroup };
		WalkTree(visitor);
		mGeometry = new DebugRenderer::Geometry(inRenderer->CreateTriangleBatch(triangles), GetLocalBounds());
	}

	// Test if the shape is scaled inside out
	DebugRenderer::ECullMode cull_mode = ScaleHelpers::IsInsideOut(inScale)? DebugRenderer::ECullMode::CullFrontFace : DebugRenderer::ECullMode::CullBackFace;

	// Determine the draw mode
	DebugRenderer::EDrawMode draw_mode = inDrawWireframe? DebugRenderer::EDrawMode::Wireframe : DebugRenderer::EDrawMode::Solid;

	// Draw the geometry
	inRenderer->DrawGeometry(inCenterOfMassTransform * Mat44::sScale(inScale), inColor, mGeometry, cull_mode, DebugRenderer::ECastShadow::On, draw_mode);

	if (sDrawTriangleOutlines)
	{
		struct Visitor
		{
					Visitor(DebugRenderer *inRenderer, Mat44Arg inTransform) :
				mRenderer(inRenderer),
				mTransform(inTransform)
			{
			}

			bool	ShouldAbort() const
			{
				return false;
			}

			bool	ShouldVisitNode(int inStackTop) const
			{
				return true;
			}

			int		VisitNodes(Vec4Arg inBoundsMinX, Vec4Arg inBoundsMinY, Vec4Arg inBoundsMinZ, Vec4Arg inBoundsMaxX, Vec4Arg inBoundsMaxY, Vec4Arg inBoundsMaxZ, UVec4 &ioProperties, int inStackTop) 
			{
				UVec4 valid = UVec4::sOr(UVec4::sOr(Vec4::sLess(inBoundsMinX, inBoundsMaxX), Vec4::sLess(inBoundsMinY, inBoundsMaxY)), Vec4::sLess(inBoundsMinZ, inBoundsMaxZ));
				UVec4::sSort4True(valid, ioProperties);
				return valid.CountTrues();
			}

			void	VisitTriangles(const TriangleCodec::DecodingContext &ioContext, Vec3Arg inRootBoundsMin, Vec3Arg inRootBoundsMax, const void *inTriangles, int inNumTriangles, uint32 inTriangleBlockID) 
			{
				// Get vertices
				JPH_ASSERT(inNumTriangles <= MaxTrianglesPerLeaf);
				Vec3 vertices[MaxTrianglesPerLeaf * 3];
				ioContext.Unpack(inRootBoundsMin, inRootBoundsMax, inTriangles, inNumTriangles, vertices);

				// Get flags
				uint8 flags[MaxTrianglesPerLeaf];
				TriangleCodec::DecodingContext::sGetFlags(inTriangles, inNumTriangles, flags);

				// Loop through triangles
				const uint8 *f = flags;
				for (Vec3 *v = vertices, *v_end = vertices + inNumTriangles * 3; v < v_end; v += 3, ++f)
				{
					// Loop through edges
					for (uint edge_idx = 0; edge_idx < 3; ++edge_idx)
					{
						Vec3 v1 = mTransform * v[edge_idx];
						Vec3 v2 = mTransform * v[(edge_idx + 1) % 3];

						// Draw active edge as a green arrow, other edges as grey
						if (*f & (1 << (edge_idx + FLAGS_ACTIVE_EGDE_SHIFT)))
							mRenderer->DrawArrow(v1, v2, Color::sGreen, 0.01f);
						else
							mRenderer->DrawLine(v1, v2, Color::sGrey);
					}
				}
			}

			DebugRenderer *	mRenderer;
			Mat44			mTransform;
		};

		Visitor visitor { inRenderer, inCenterOfMassTransform * Mat44::sScale(inScale) };
		WalkTree(visitor);
	}
}
#endif // JPH_DEBUG_RENDERER

bool MeshShape::CastRay(const RayCast &inRay, const SubShapeIDCreator &inSubShapeIDCreator, RayCastResult &ioHit) const
{
	JPH_PROFILE_FUNCTION();

	struct Visitor
	{
				Visitor(RayCastResult &ioHit) : 
			mHit(ioHit)
		{
		}

		bool	ShouldAbort() const
		{
			return mHit.mFraction <= 0.0f;
		}

		bool	ShouldVisitNode(int inStackTop) const
		{
			return mDistanceStack[inStackTop] < mHit.mFraction;
		}

		int		VisitNodes(Vec4Arg inBoundsMinX, Vec4Arg inBoundsMinY, Vec4Arg inBoundsMinZ, Vec4Arg inBoundsMaxX, Vec4Arg inBoundsMaxY, Vec4Arg inBoundsMaxZ, UVec4 &ioProperties, int inStackTop) 
		{
			// Test bounds of 4 children
			Vec4 distance = RayAABox4(mRayOrigin, mRayInvDirection, inBoundsMinX, inBoundsMinY, inBoundsMinZ, inBoundsMaxX, inBoundsMaxY, inBoundsMaxZ);
	
			// Sort so that highest values are first (we want to first process closer hits and we process stack top to bottom)
			Vec4::sSort4Reverse(distance, ioProperties);

			// Count how many results are closer
			UVec4 closer = Vec4::sLess(distance, Vec4::sReplicate(mHit.mFraction));
			int num_results = closer.CountTrues();

			// Shift the results so that only the closer ones remain
			distance = distance.ReinterpretAsInt().ShiftComponents4Minus(num_results).ReinterpretAsFloat();
			ioProperties = ioProperties.ShiftComponents4Minus(num_results);

			distance.StoreFloat4((Float4 *)&mDistanceStack[inStackTop]);
			return num_results;
		}

		void	VisitTriangles(const TriangleCodec::DecodingContext &ioContext, Vec3Arg inRootBoundsMin, Vec3Arg inRootBoundsMax, const void *inTriangles, int inNumTriangles, uint32 inTriangleBlockID) 
		{
			// Test against triangles
			uint32 triangle_idx;
			float fraction = ioContext.TestRay(mRayOrigin, mRayDirection, inRootBoundsMin, inRootBoundsMax, inTriangles, inNumTriangles, mHit.mFraction, triangle_idx);
			if (fraction < mHit.mFraction)
			{
				mHit.mFraction = fraction;
				mHit.mSubShapeID2 = mSubShapeIDCreator.PushID(inTriangleBlockID, mTriangleBlockIDBits).PushID(triangle_idx, NumTriangleBits).GetID();
				mReturnValue = true;
			}
		}

		RayCastResult &		mHit;
		Vec3				mRayOrigin;
		Vec3				mRayDirection;
		RayInvDirection		mRayInvDirection;
		uint				mTriangleBlockIDBits;
		SubShapeIDCreator	mSubShapeIDCreator;
		bool				mReturnValue = false;
		float				mDistanceStack[NodeCodec::StackSize];
	};

	Visitor visitor(ioHit);

	visitor.mRayOrigin = inRay.mOrigin;
	visitor.mRayDirection = inRay.mDirection;
	visitor.mRayInvDirection.Set(inRay.mDirection);
	visitor.mTriangleBlockIDBits = NodeCodec::DecodingContext::sTriangleBlockIDBits(mTree);
	visitor.mSubShapeIDCreator = inSubShapeIDCreator;

	WalkTree(visitor);

	return visitor.mReturnValue;
}

void MeshShape::CastRay(const RayCast &inRay, const RayCastSettings &inRayCastSettings, const SubShapeIDCreator &inSubShapeIDCreator, CastRayCollector &ioCollector) const
{
	JPH_PROFILE_FUNCTION();

	struct Visitor
	{
				Visitor(CastRayCollector &ioCollector) : 
			mCollector(ioCollector)
		{
		}

		bool	ShouldAbort() const
		{
			return mCollector.ShouldEarlyOut();
		}

		bool	ShouldVisitNode(int inStackTop) const
		{
			return mDistanceStack[inStackTop] < mCollector.GetEarlyOutFraction();
		}

		int		VisitNodes(Vec4Arg inBoundsMinX, Vec4Arg inBoundsMinY, Vec4Arg inBoundsMinZ, Vec4Arg inBoundsMaxX, Vec4Arg inBoundsMaxY, Vec4Arg inBoundsMaxZ, UVec4 &ioProperties, int inStackTop) 
		{
			// Test bounds of 4 children
			Vec4 distance = RayAABox4(mRayOrigin, mRayInvDirection, inBoundsMinX, inBoundsMinY, inBoundsMinZ, inBoundsMaxX, inBoundsMaxY, inBoundsMaxZ);
	
			// Sort so that highest values are first (we want to first process closer hits and we process stack top to bottom)
			Vec4::sSort4Reverse(distance, ioProperties);

			// Count how many results are closer
			UVec4 closer = Vec4::sLess(distance, Vec4::sReplicate(mCollector.GetEarlyOutFraction()));
			int num_results = closer.CountTrues();

			// Shift the results so that only the closer ones remain
			distance = distance.ReinterpretAsInt().ShiftComponents4Minus(num_results).ReinterpretAsFloat();
			ioProperties = ioProperties.ShiftComponents4Minus(num_results);

			distance.StoreFloat4((Float4 *)&mDistanceStack[inStackTop]);
			return num_results;
		}

		void	VisitTriangles(const TriangleCodec::DecodingContext &ioContext, Vec3Arg inRootBoundsMin, Vec3Arg inRootBoundsMax, const void *inTriangles, int inNumTriangles, uint32 inTriangleBlockID) 
		{
			// Create ID for triangle block
			SubShapeIDCreator block_sub_shape_id = mSubShapeIDCreator.PushID(inTriangleBlockID, mTriangleBlockIDBits);

			// Decode vertices
			JPH_ASSERT(inNumTriangles <= MaxTrianglesPerLeaf);
			Vec3 vertices[MaxTrianglesPerLeaf * 3];
			ioContext.Unpack(inRootBoundsMin, inRootBoundsMax, inTriangles, inNumTriangles, vertices);

			// Decode triangle flags
			uint8 flags[MaxTrianglesPerLeaf];
			TriangleCodec::DecodingContext::sGetFlags(inTriangles, inNumTriangles, flags);

			// Loop over all triangles
			for (int triangle_idx = 0; triangle_idx < inNumTriangles; ++triangle_idx)
			{
				// Determine vertices
				const Vec3 *vertex = vertices + triangle_idx * 3;
				Vec3 v0 = vertex[0];
				Vec3 v1 = vertex[1];
				Vec3 v2 = vertex[2];

				// Back facing check
				if (mBackFaceMode == EBackFaceMode::IgnoreBackFaces && (v2 - v0).Cross(v1 - v0).Dot(mRayDirection) < 0)
					continue;

				// Check the triangle
				float fraction = RayTriangle(mRayOrigin, mRayDirection, v0, v1, v2);
				if (fraction < mCollector.GetEarlyOutFraction())
				{
					RayCastResult hit;
					hit.mBodyID = TransformedShape::sGetBodyID(mCollector.GetContext());
					hit.mFraction = fraction;
					hit.mSubShapeID2 = block_sub_shape_id.PushID(triangle_idx, NumTriangleBits).GetID();
					mCollector.AddHit(hit);
				}
			}
		}

		CastRayCollector &		mCollector;
		Vec3					mRayOrigin;
		Vec3					mRayDirection;
		RayInvDirection			mRayInvDirection;
		EBackFaceMode			mBackFaceMode;
		uint					mTriangleBlockIDBits;
		SubShapeIDCreator		mSubShapeIDCreator;
		float					mDistanceStack[NodeCodec::StackSize];
	};

	Visitor visitor(ioCollector);
	visitor.mBackFaceMode = inRayCastSettings.mBackFaceMode;
	visitor.mRayOrigin = inRay.mOrigin;
	visitor.mRayDirection = inRay.mDirection;
	visitor.mRayInvDirection.Set(inRay.mDirection);
	visitor.mTriangleBlockIDBits = NodeCodec::DecodingContext::sTriangleBlockIDBits(mTree);
	visitor.mSubShapeIDCreator = inSubShapeIDCreator;

	WalkTree(visitor);
}

void MeshShape::CollidePoint(Vec3Arg inPoint, const SubShapeIDCreator &inSubShapeIDCreator, CollidePointCollector &ioCollector) const
{
	// First test if we're inside our bounding box
	AABox bounds = GetLocalBounds();
	if (bounds.Contains(inPoint))
	{
		// A collector that just counts the number of hits
		class HitCountCollector : public CastRayCollector	
		{
		public:
			virtual void	AddHit(const RayCastResult &inResult) override
			{
				// Store the last sub shape ID so that we can provide something to our outer hit collector
				mSubShapeID = inResult.mSubShapeID2;

				++mHitCount;
			}

			int				mHitCount = 0;
			SubShapeID		mSubShapeID;
		};
		HitCountCollector collector;

		// Configure the raycast
		RayCastSettings settings;
		settings.mBackFaceMode = EBackFaceMode::CollideWithBackFaces;

		// Cast a ray that's 10% longer than the heigth of our bounding box
		CastRay(RayCast { inPoint, 1.1f * bounds.GetSize().GetY() * Vec3::sAxisY() }, settings, inSubShapeIDCreator, collector);

		// Odd amount of hits means inside
		if ((collector.mHitCount & 1) == 1)
			ioCollector.AddHit({ TransformedShape::sGetBodyID(ioCollector.GetContext()), collector.mSubShapeID });
	}
}

void MeshShape::CastShape(const ShapeCast &inShapeCast, const ShapeCastSettings &inShapeCastSettings, Vec3Arg inScale, const ShapeFilter &inShapeFilter, Mat44Arg inCenterOfMassTransform2, const SubShapeIDCreator &inSubShapeIDCreator1, const SubShapeIDCreator &inSubShapeIDCreator2, CastShapeCollector &ioCollector) const 
{
	JPH_PROFILE_FUNCTION();

	struct Visitor : public CastConvexVsTriangles
	{
		using CastConvexVsTriangles::CastConvexVsTriangles;

		bool		ShouldAbort() const
		{
			return mCollector.ShouldEarlyOut();
		}

		bool		ShouldVisitNode(int inStackTop) const
		{
			return mDistanceStack[inStackTop] < mCollector.GetEarlyOutFraction();
		}

		int			VisitNodes(Vec4Arg inBoundsMinX, Vec4Arg inBoundsMinY, Vec4Arg inBoundsMinZ, Vec4Arg inBoundsMaxX, Vec4Arg inBoundsMaxY, Vec4Arg inBoundsMaxZ, UVec4 &ioProperties, int inStackTop) 
		{
			// Scale the bounding boxes of this node
			Vec4 bounds_min_x, bounds_min_y, bounds_min_z, bounds_max_x, bounds_max_y, bounds_max_z;
			AABox4Scale(mScale, inBoundsMinX, inBoundsMinY, inBoundsMinZ, inBoundsMaxX, inBoundsMaxY, inBoundsMaxZ, bounds_min_x, bounds_min_y, bounds_min_z, bounds_max_x, bounds_max_y, bounds_max_z);

			// Enlarge them by the casted shape's box extents
			AABox4EnlargeWithExtent(mBoxExtent, bounds_min_x, bounds_min_y, bounds_min_z, bounds_max_x, bounds_max_y, bounds_max_z);

			// Test bounds of 4 children
			Vec4 distance = RayAABox4(mBoxCenter, mInvDirection, bounds_min_x, bounds_min_y, bounds_min_z, bounds_max_x, bounds_max_y, bounds_max_z);
	
			// Sort so that highest values are first (we want to first process closer hits and we process stack top to bottom)
			Vec4::sSort4Reverse(distance, ioProperties);

			// Count how many results are closer
			UVec4 closer = Vec4::sLess(distance, Vec4::sReplicate(mCollector.GetEarlyOutFraction()));
			int num_results = closer.CountTrues();

			// Shift the results so that only the closer ones remain
			distance = distance.ReinterpretAsInt().ShiftComponents4Minus(num_results).ReinterpretAsFloat();
			ioProperties = ioProperties.ShiftComponents4Minus(num_results);

			distance.StoreFloat4((Float4 *)&mDistanceStack[inStackTop]);
			return num_results;
		}

		void		VisitTriangles(const TriangleCodec::DecodingContext &ioContext, Vec3Arg inRootBoundsMin, Vec3Arg inRootBoundsMax, const void *inTriangles, int inNumTriangles, uint32 inTriangleBlockID) 
		{
			// Create ID for triangle block
			SubShapeIDCreator block_sub_shape_id = mSubShapeIDCreator2.PushID(inTriangleBlockID, mTriangleBlockIDBits);

			// Decode vertices
			JPH_ASSERT(inNumTriangles <= MaxTrianglesPerLeaf);
			Vec3 vertices[MaxTrianglesPerLeaf * 3];
			ioContext.Unpack(inRootBoundsMin, inRootBoundsMax, inTriangles, inNumTriangles, vertices);

			// Decode triangle flags
			uint8 flags[MaxTrianglesPerLeaf];
			TriangleCodec::DecodingContext::sGetFlags(inTriangles, inNumTriangles, flags);

			int triangle_idx = 0;
			for (Vec3 *v = vertices, *v_end = vertices + inNumTriangles * 3; v < v_end; v += 3, triangle_idx++)
			{
				// Determine active edges
				uint8 active_edges = (flags[triangle_idx] >> FLAGS_ACTIVE_EGDE_SHIFT) & FLAGS_ACTIVE_EDGE_MASK;

				// Create ID for triangle
				SubShapeIDCreator triangle_sub_shape_id = block_sub_shape_id.PushID(triangle_idx, NumTriangleBits);

				Cast(v[0], v[1], v[2], active_edges, triangle_sub_shape_id.GetID());
				
				// Check if we should exit because we found our hit
				if (mCollector.ShouldEarlyOut())
					break;
			}
		}

		RayInvDirection				mInvDirection;
		Vec3						mBoxCenter;
		Vec3						mBoxExtent;
		SubShapeIDCreator			mSubShapeIDCreator2;
		uint						mTriangleBlockIDBits;
		float						mDistanceStack[NodeCodec::StackSize];
	};

	Visitor visitor(inShapeCast, inShapeCastSettings, inScale, inShapeFilter, inCenterOfMassTransform2, inSubShapeIDCreator1, ioCollector);
	visitor.mInvDirection.Set(inShapeCast.mDirection);
	visitor.mBoxCenter = inShapeCast.mShapeWorldBounds.GetCenter();
	visitor.mBoxExtent = inShapeCast.mShapeWorldBounds.GetExtent();
	visitor.mSubShapeIDCreator2 = inSubShapeIDCreator2;
	visitor.mTriangleBlockIDBits = NodeCodec::DecodingContext::sTriangleBlockIDBits(mTree);
	WalkTree(visitor);
}

struct MeshShape::MSGetTrianglesContext
{
			MSGetTrianglesContext(const MeshShape *inShape, const AABox &inBox, Vec3Arg inPositionCOM, QuatArg inRotation, Vec3Arg inScale) : 
		mDecodeCtx(sGetNodeHeader(inShape->mTree)),
		mShape(inShape),
		mLocalBox(Mat44::sInverseRotationTranslation(inRotation, inPositionCOM), inBox),
		mMeshScale(inScale),
		mLocalToWorld(Mat44::sRotationTranslation(inRotation, inPositionCOM) * Mat44::sScale(inScale)),
		mIsInsideOut(ScaleHelpers::IsInsideOut(inScale))
	{
	}

	bool	ShouldAbort() const
	{
		return mShouldAbort;
	}

	bool	ShouldVisitNode(int inStackTop) const
	{
		return true;
	}

	int		VisitNodes(Vec4Arg inBoundsMinX, Vec4Arg inBoundsMinY, Vec4Arg inBoundsMinZ, Vec4Arg inBoundsMaxX, Vec4Arg inBoundsMaxY, Vec4Arg inBoundsMaxZ, UVec4 &ioProperties, int inStackTop) 
	{
		// Scale the bounding boxes of this node
		Vec4 bounds_min_x, bounds_min_y, bounds_min_z, bounds_max_x, bounds_max_y, bounds_max_z;
		AABox4Scale(mMeshScale, inBoundsMinX, inBoundsMinY, inBoundsMinZ, inBoundsMaxX, inBoundsMaxY, inBoundsMaxZ, bounds_min_x, bounds_min_y, bounds_min_z, bounds_max_x, bounds_max_y, bounds_max_z);

		// Test which nodes collide
		UVec4 collides = AABox4VsBox(mLocalBox, bounds_min_x, bounds_min_y, bounds_min_z, bounds_max_x, bounds_max_y, bounds_max_z);

		// Sort so the colliding ones go first
		UVec4::sSort4True(collides, ioProperties);

		// Return number of hits
		return collides.CountTrues();
	}

	void	VisitTriangles(const TriangleCodec::DecodingContext &ioContext, Vec3Arg inRootBoundsMin, Vec3Arg inRootBoundsMax, const void *inTriangles, int inNumTriangles, uint32 inTriangleBlockID) 
	{
		// When the buffer is full and we cannot process the triangles, abort the tree walk. The next time GetTrianglesNext is called we will continue here.
		if (mNumTrianglesFound + inNumTriangles > mMaxTrianglesRequested)
		{
			mShouldAbort = true;
			return;
		}

		// Decode vertices
		JPH_ASSERT(inNumTriangles <= MaxTrianglesPerLeaf);
		Vec3 vertices[MaxTrianglesPerLeaf * 3];
		ioContext.Unpack(inRootBoundsMin, inRootBoundsMax, inTriangles, inNumTriangles, vertices);

		// Store vertices as Float3
		if (mIsInsideOut)
		{
			// Scaled inside out, flip the triangles
			for (Vec3 *v = vertices, *v_end = v + 3 * inNumTriangles; v < v_end; v += 3)
			{
				(mLocalToWorld * v[0]).StoreFloat3(mTriangleVertices++);
				(mLocalToWorld * v[2]).StoreFloat3(mTriangleVertices++);
				(mLocalToWorld * v[1]).StoreFloat3(mTriangleVertices++);
			}
		}
		else
		{
			// Normal scale
			for (Vec3 *v = vertices, *v_end = v + 3 * inNumTriangles; v < v_end; ++v)
				(mLocalToWorld * *v).StoreFloat3(mTriangleVertices++);
		}

		if (mMaterials != nullptr)
		{
			if (mShape->mMaterials.empty())
			{
				// No materials, output default
				const PhysicsMaterial *default_material = PhysicsMaterial::sDefault;
				for (int m = 0; m < inNumTriangles; ++m)
					*mMaterials++ = default_material;
			}
			else
			{
				// Decode triangle flags
				uint8 flags[MaxTrianglesPerLeaf];
				TriangleCodec::DecodingContext::sGetFlags(inTriangles, inNumTriangles, flags);
	
				// Store materials
				for (const uint8 *f = flags, *f_end = f + inNumTriangles; f < f_end; ++f)
					*mMaterials++ = mShape->mMaterials[*f & FLAGS_MATERIAL_MASK].GetPtr();
			}
		}

		// Accumulate triangles found
		mNumTrianglesFound += inNumTriangles;
	}

	NodeCodec::DecodingContext	mDecodeCtx;
	const MeshShape *			mShape;
	OrientedBox					mLocalBox;
	Vec3						mMeshScale;
	Mat44						mLocalToWorld;
	int							mMaxTrianglesRequested;
	Float3 *					mTriangleVertices;
	int							mNumTrianglesFound;
	const PhysicsMaterial **	mMaterials;
	bool						mShouldAbort;
	bool						mIsInsideOut;
};

void MeshShape::GetTrianglesStart(GetTrianglesContext &ioContext, const AABox &inBox, Vec3Arg inPositionCOM, QuatArg inRotation, Vec3Arg inScale) const
{
	static_assert(sizeof(MSGetTrianglesContext) <= sizeof(GetTrianglesContext), "GetTrianglesContext too small");
	JPH_ASSERT(IsAligned(&ioContext, alignof(MSGetTrianglesContext)));

	new (&ioContext) MSGetTrianglesContext(this, inBox, inPositionCOM, inRotation, inScale);
}

int MeshShape::GetTrianglesNext(GetTrianglesContext &ioContext, int inMaxTrianglesRequested, Float3 *outTriangleVertices, const PhysicsMaterial **outMaterials) const
{
	static_assert(cGetTrianglesMinTrianglesRequested >= MaxTrianglesPerLeaf, "cGetTrianglesMinTrianglesRequested is too small");
	JPH_ASSERT(inMaxTrianglesRequested >= cGetTrianglesMinTrianglesRequested);

	// Check if we're done
	MSGetTrianglesContext &context = (MSGetTrianglesContext &)ioContext;
	if (context.mDecodeCtx.IsDoneWalking())
		return 0;

	// Store parameters on context
	context.mMaxTrianglesRequested = inMaxTrianglesRequested;
	context.mTriangleVertices = outTriangleVertices;
	context.mMaterials = outMaterials;
	context.mShouldAbort = false; // Reset the abort flag
	context.mNumTrianglesFound = 0;
	
	// Continue (or start) walking the tree
	const TriangleCodec::DecodingContext triangle_ctx(sGetTriangleHeader(mTree), mTree);
	const uint8 *buffer_start = &mTree[0];
	context.mDecodeCtx.WalkTree(buffer_start, triangle_ctx, context);
	return context.mNumTrianglesFound;
}

void MeshShape::sCollideConvexVsMesh(const Shape *inShape1, const Shape *inShape2, Vec3Arg inScale1, Vec3Arg inScale2, Mat44Arg inCenterOfMassTransform1, Mat44Arg inCenterOfMassTransform2, const SubShapeIDCreator &inSubShapeIDCreator1, const SubShapeIDCreator &inSubShapeIDCreator2, const CollideShapeSettings &inCollideShapeSettings, CollideShapeCollector &ioCollector)
{
	JPH_PROFILE_FUNCTION();

	// Get the shapes
	JPH_ASSERT(inShape1->GetType() == EShapeType::Convex);
	JPH_ASSERT(inShape2->GetType() == EShapeType::Mesh);
	const ConvexShape *shape1 = static_cast<const ConvexShape *>(inShape1);
	const MeshShape *shape2 = static_cast<const MeshShape *>(inShape2);

	struct Visitor : public CollideConvexVsTriangles
	{
		using CollideConvexVsTriangles::CollideConvexVsTriangles;

		bool	ShouldAbort() const
		{
			return mCollector.ShouldEarlyOut();
		}

		bool	ShouldVisitNode(int inStackTop) const
		{
			return true;
		}

		int		VisitNodes(Vec4Arg inBoundsMinX, Vec4Arg inBoundsMinY, Vec4Arg inBoundsMinZ, Vec4Arg inBoundsMaxX, Vec4Arg inBoundsMaxY, Vec4Arg inBoundsMaxZ, UVec4 &ioProperties, int inStackTop) 
		{
			// Scale the bounding boxes of this node
			Vec4 bounds_min_x, bounds_min_y, bounds_min_z, bounds_max_x, bounds_max_y, bounds_max_z;
			AABox4Scale(mScale2, inBoundsMinX, inBoundsMinY, inBoundsMinZ, inBoundsMaxX, inBoundsMaxY, inBoundsMaxZ, bounds_min_x, bounds_min_y, bounds_min_z, bounds_max_x, bounds_max_y, bounds_max_z);

			// Test which nodes collide
			UVec4 collides = AABox4VsBox(mBoundsOf1InSpaceOf2, bounds_min_x, bounds_min_y, bounds_min_z, bounds_max_x, bounds_max_y, bounds_max_z);

			// Sort so the colliding ones go first
			UVec4::sSort4True(collides, ioProperties);

			// Return number of hits
			return collides.CountTrues();
		}

		void	VisitTriangles(const TriangleCodec::DecodingContext &ioContext, Vec3Arg inRootBoundsMin, Vec3Arg inRootBoundsMax, const void *inTriangles, int inNumTriangles, uint32 inTriangleBlockID) 
		{
			// Create ID for triangle block
			SubShapeIDCreator block_sub_shape_id = mSubShapeIDCreator2.PushID(inTriangleBlockID, mTriangleBlockIDBits);

			// Decode vertices
			JPH_ASSERT(inNumTriangles <= MaxTrianglesPerLeaf);
			Vec3 vertices[MaxTrianglesPerLeaf * 3];
			ioContext.Unpack(inRootBoundsMin, inRootBoundsMax, inTriangles, inNumTriangles, vertices);

			// Decode triangle flags
			uint8 flags[MaxTrianglesPerLeaf];
			TriangleCodec::DecodingContext::sGetFlags(inTriangles, inNumTriangles, flags);

			// Loop over all triangles
			for (int triangle_idx = 0; triangle_idx < inNumTriangles; ++triangle_idx)
			{
				// Create ID for triangle
				SubShapeID triangle_sub_shape_id = block_sub_shape_id.PushID(triangle_idx, NumTriangleBits).GetID();

				// Determine active edges
				uint8 active_edges = (flags[triangle_idx] >> FLAGS_ACTIVE_EGDE_SHIFT) & FLAGS_ACTIVE_EDGE_MASK;

				// Determine vertices
				const Vec3 *vertex = vertices + triangle_idx * 3;

				Collide(vertex[0], vertex[1], vertex[2], active_edges, triangle_sub_shape_id);

				// Check if we should exit because we found our hit
				if (mCollector.ShouldEarlyOut())
					break;
			}
		}

		uint							mTriangleBlockIDBits;
		SubShapeIDCreator				mSubShapeIDCreator2;
	};

	Visitor visitor(shape1, inScale1, inScale2, inCenterOfMassTransform1, inCenterOfMassTransform2, inSubShapeIDCreator1.GetID(), inCollideShapeSettings, ioCollector);
	visitor.mTriangleBlockIDBits = NodeCodec::DecodingContext::sTriangleBlockIDBits(shape2->mTree);
	visitor.mSubShapeIDCreator2 = inSubShapeIDCreator2;

	shape2->WalkTree(visitor);
}

void MeshShape::SaveBinaryState(StreamOut &inStream) const
{
	Shape::SaveBinaryState(inStream);

	inStream.Write(static_cast<const ByteBufferVector &>(mTree)); // Make sure we use the vector<> overload
}

void MeshShape::RestoreBinaryState(StreamIn &inStream)
{
	Shape::RestoreBinaryState(inStream);

	inStream.Read(static_cast<ByteBufferVector &>(mTree)); // Make sure we use the vector<> overload
}

void MeshShape::SaveMaterialState(PhysicsMaterialList &outMaterials) const
{ 
	outMaterials = mMaterials;
}

void MeshShape::RestoreMaterialState(const PhysicsMaterialRefC *inMaterials, uint inNumMaterials) 
{ 
	mMaterials.assign(inMaterials, inMaterials + inNumMaterials);
}

Shape::Stats MeshShape::GetStats() const
{
	// Walk the tree to count the triangles
	struct Visitor
	{
		bool		ShouldAbort() const
		{
			return false;
		}

		bool		ShouldVisitNode(int inStackTop) const
		{
			return true;
		}

		int			VisitNodes(Vec4Arg inBoundsMinX, Vec4Arg inBoundsMinY, Vec4Arg inBoundsMinZ, Vec4Arg inBoundsMaxX, Vec4Arg inBoundsMaxY, Vec4Arg inBoundsMaxZ, UVec4 &ioProperties, int inStackTop) 
		{
			// Visit all valid children
			UVec4 valid = UVec4::sOr(UVec4::sOr(Vec4::sLess(inBoundsMinX, inBoundsMaxX), Vec4::sLess(inBoundsMinY, inBoundsMaxY)), Vec4::sLess(inBoundsMinZ, inBoundsMaxZ));
			UVec4::sSort4True(valid, ioProperties);
			return valid.CountTrues();
		}

		void		VisitTriangles(const TriangleCodec::DecodingContext &ioContext, Vec3Arg inRootBoundsMin, Vec3Arg inRootBoundsMax, const void *inTriangles, int inNumTriangles, uint32 inTriangleBlockID) 
		{
			mNumTriangles += inNumTriangles;
		}

		uint		mNumTriangles = 0;
	};
	Visitor visitor;
	WalkTree(visitor);
	
	return Stats(sizeof(*this) + mMaterials.size() * sizeof(Ref<PhysicsMaterial>) + mTree.size() * sizeof(uint8), visitor.mNumTriangles);
}

void MeshShape::sRegister()
{
	ShapeFunctions &f = ShapeFunctions::sGet(EShapeSubType::Mesh);
	f.mConstruct = []() -> Shape * { return new MeshShape; };
	f.mColor = Color::sRed;

	for (EShapeSubType s : sConvexSubShapeTypes)
		CollisionDispatch::sRegisterCollideShape(s, EShapeSubType::Mesh, sCollideConvexVsMesh);
}

} // JPH