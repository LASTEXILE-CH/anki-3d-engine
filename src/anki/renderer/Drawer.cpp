// Copyright (C) 2009-2017, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <anki/renderer/Drawer.h>
#include <anki/resource/ShaderResource.h>
#include <anki/scene/FrustumComponent.h>
#include <anki/resource/Material.h>
#include <anki/scene/RenderComponent.h>
#include <anki/scene/Visibility.h>
#include <anki/scene/SceneGraph.h>
#include <anki/resource/TextureResource.h>
#include <anki/renderer/Renderer.h>
#include <anki/core/Trace.h>
#include <anki/util/Logger.h>

namespace anki
{

/// Drawer's context
class DrawContext
{
public:
	RenderQueueDrawContext m_queueCtx;

	const VisibleNode* m_visibleNode = nullptr;

	Array<RenderQueueElement, MAX_INSTANCES> m_cachedRenderElements;
	Array<U8, MAX_INSTANCES> m_cachedRenderElementLods;
	U m_cachedRenderElementCount = 0;
};

/// Check if the drawcalls can be merged.
static Bool canMergeRenderQueueElements(const RenderQueueElement& a, const RenderQueueElement& b)
{
	return a.m_callback == b.m_callback && a.m_mergeKey != 0 && a.m_mergeKey == b.m_mergeKey;
}

RenderableDrawer::~RenderableDrawer()
{
}

void RenderableDrawer::drawRange(Pass pass,
	const Mat4& viewMat,
	const Mat4& viewProjMat,
	CommandBufferPtr cmdb,
	const VisibleNode* begin,
	const VisibleNode* end)
{
	ANKI_ASSERT(begin && end && begin < end);

	DrawContext ctx;
	ctx.m_queueCtx.m_viewMatrix = viewMat;
	ctx.m_queueCtx.m_viewProjectionMatrix = viewProjMat;
	ctx.m_queueCtx.m_projectionMatrix = Mat4::getIdentity(); // TODO
	ctx.m_queueCtx.m_cameraTransform = ctx.m_queueCtx.m_viewMatrix.getInverse();
	ctx.m_queueCtx.m_stagingGpuAllocator = &m_r->getStagingGpuMemoryManager();
	ctx.m_queueCtx.m_commandBuffer = cmdb;
	ctx.m_queueCtx.m_key = RenderingKey(pass, 0, 1);

	for(; begin != end; ++begin)
	{
		ctx.m_visibleNode = begin;

		drawSingle(ctx);
	}

	// Flush the last drawcall
	flushDrawcall(ctx);
}

void RenderableDrawer::flushDrawcall(DrawContext& ctx)
{
	ctx.m_queueCtx.m_key.m_lod = ctx.m_cachedRenderElementLods[0];
	ctx.m_queueCtx.m_key.m_instanceCount = ctx.m_cachedRenderElementCount;

	ctx.m_cachedRenderElements[0].m_callback(ctx.m_queueCtx,
		WeakArray<const RenderQueueElement>(&ctx.m_cachedRenderElements[0], ctx.m_cachedRenderElementCount));

	// Rendered something, reset the cached transforms
	if(ctx.m_cachedRenderElementCount > 1)
	{
		ANKI_TRACE_INC_COUNTER(RENDERER_MERGED_DRAWCALLS, ctx.m_cachedRenderElementCount - 1);
	}
	ctx.m_cachedRenderElementCount = 0;
}

void RenderableDrawer::drawSingle(DrawContext& ctx)
{
	const RenderComponent& rc = ctx.m_visibleNode->m_node->getComponent<RenderComponent>();

	if(ctx.m_cachedRenderElementCount == MAX_INSTANCES)
	{
		flushDrawcall(ctx);
	}

	RenderQueueElement rqel;
	rc.setupRenderQueueElement(rqel);

	const F32 flod = min<F32>(m_r->calculateLod(ctx.m_visibleNode->m_frustumDistance), MAX_LOD_COUNT - 1);
	const U8 lod = U8(flod);

	const Bool shouldFlush = ctx.m_cachedRenderElementCount > 0
		&& (!canMergeRenderQueueElements(ctx.m_cachedRenderElements[ctx.m_cachedRenderElementCount - 1], rqel)
			   || ctx.m_cachedRenderElementLods[ctx.m_cachedRenderElementCount - 1] != lod);

	if(shouldFlush)
	{
		flushDrawcall(ctx);
	}

	// Cache the new one
	ctx.m_cachedRenderElements[ctx.m_cachedRenderElementCount] = rqel;
	ctx.m_cachedRenderElementLods[ctx.m_cachedRenderElementCount] = lod;
	++ctx.m_cachedRenderElementCount;
}

} // end namespace anki
