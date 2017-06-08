// Copyright (C) 2009-2017, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <anki/renderer/Renderer.h>
#include <anki/scene/SceneGraph.h>
#include <anki/scene/FrustumComponent.h>
#include <anki/core/Trace.h>
#include <anki/misc/ConfigSet.h>

#include <anki/renderer/Indirect.h>
#include <anki/renderer/GBuffer.h>
#include <anki/renderer/LightShading.h>
#include <anki/renderer/ShadowMapping.h>
#include <anki/renderer/FinalComposite.h>
#include <anki/renderer/Ssao.h>
#include <anki/renderer/Bloom.h>
#include <anki/renderer/Tonemapping.h>
#include <anki/renderer/ForwardShading.h>
#include <anki/renderer/LensFlare.h>
#include <anki/renderer/Dbg.h>
#include <anki/renderer/DownscaleBlur.h>
#include <anki/renderer/Volumetric.h>
#include <anki/renderer/DepthDownscale.h>
#include <anki/renderer/TemporalAA.h>

#include <cstdarg> // For var args

namespace anki
{

static Bool threadWillDoWork(
	const RenderingContext& ctx, VisibilityGroupType typeOfWork, U32 threadId, PtrSize threadCount)
{
	U problemSize = ctx.m_visResults->getCount(typeOfWork);
	PtrSize start, end;
	ThreadPoolTask::choseStartEnd(threadId, threadCount, problemSize, start, end);

	return start != end;
}

Renderer::Renderer()
	: m_sceneDrawer(this)
{
}

Renderer::~Renderer()
{
}

Error Renderer::init(ThreadPool* threadpool,
	ResourceManager* resources,
	GrManager* gl,
	StagingGpuMemoryManager* stagingMem,
	HeapAllocator<U8> alloc,
	StackAllocator<U8> frameAlloc,
	const ConfigSet& config,
	Timestamp* globTimestamp,
	Bool willDrawToDefaultFbo)
{
	m_globTimestamp = globTimestamp;
	m_threadpool = threadpool;
	m_resources = resources;
	m_gr = gl;
	m_stagingMem = stagingMem;
	m_alloc = alloc;
	m_frameAlloc = frameAlloc;
	m_willDrawToDefaultFbo = willDrawToDefaultFbo;

	Error err = initInternal(config);
	if(err)
	{
		ANKI_R_LOGE("Failed to initialize the renderer");
	}

	return err;
}

Error Renderer::initInternal(const ConfigSet& config)
{
	// Set from the config
	m_width = config.getNumber("width");
	m_height = config.getNumber("height");
	ANKI_R_LOGI("Initializing offscreen renderer. Size %ux%u", m_width, m_height);

	m_lodDistance = config.getNumber("lodDistance");
	m_frameCount = 0;

	m_tessellation = config.getNumber("tessellation");

	// A few sanity checks
	if(m_width < 10 || m_height < 10)
	{
		ANKI_R_LOGE("Incorrect sizes");
		return ErrorCode::USER_DATA;
	}

	{
		TextureInitInfo texinit;
		texinit.m_width = texinit.m_height = 4;
		texinit.m_usage = TextureUsageBit::SAMPLED_FRAGMENT;
		texinit.m_format = PixelFormat(ComponentFormat::R8G8B8A8, TransformFormat::UNORM);
		texinit.m_usageWhenEncountered = TextureUsageBit::SAMPLED_FRAGMENT;
		texinit.m_initialUsage = TextureUsageBit::SAMPLED_FRAGMENT;
		m_dummyTex = getGrManager().newInstance<Texture>(texinit);
	}

	m_dummyBuff = getGrManager().newInstance<Buffer>(
		getDummyBufferSize(), BufferUsageBit::UNIFORM_ALL | BufferUsageBit::STORAGE_ALL, BufferMapAccessBit::NONE);

	// quad setup
	ANKI_CHECK(m_resources->loadResource("shaders/Quad.vert.glsl", m_drawQuadVert));

	// Init the stages. Careful with the order!!!!!!!!!!
	m_indirect.reset(m_alloc.newInstance<Indirect>(this));
	ANKI_CHECK(m_indirect->init(config));

	m_gbuffer.reset(m_alloc.newInstance<GBuffer>(this));
	ANKI_CHECK(m_gbuffer->init(config));

	m_shadowMapping.reset(m_alloc.newInstance<ShadowMapping>(this));
	ANKI_CHECK(m_shadowMapping->init(config));

	m_lightShading.reset(m_alloc.newInstance<LightShading>(this));
	ANKI_CHECK(m_lightShading->init(config));

	m_depth.reset(m_alloc.newInstance<DepthDownscale>(this));
	ANKI_CHECK(m_depth->init(config));

	m_vol.reset(m_alloc.newInstance<Volumetric>(this));
	ANKI_CHECK(m_vol->init(config));

	m_forwardShading.reset(m_alloc.newInstance<ForwardShading>(this));
	ANKI_CHECK(m_forwardShading->init(config));

	m_lensFlare.reset(m_alloc.newInstance<LensFlare>(this));
	ANKI_CHECK(m_lensFlare->init(config));

	m_ssao.reset(m_alloc.newInstance<Ssao>(this));
	ANKI_CHECK(m_ssao->init(config));

	m_fsUpscale.reset(m_alloc.newInstance<ForwardShadingUpscale>(this));
	ANKI_CHECK(m_fsUpscale->init(config));

	m_downscale.reset(getAllocator().newInstance<DownscaleBlur>(this));
	ANKI_CHECK(m_downscale->init(config));

	m_tonemapping.reset(getAllocator().newInstance<Tonemapping>(this));
	ANKI_CHECK(m_tonemapping->init(config));

	m_temporalAA.reset(getAllocator().newInstance<TemporalAA>(this));
	ANKI_CHECK(m_temporalAA->init(config));

	m_bloom.reset(m_alloc.newInstance<Bloom>(this));
	ANKI_CHECK(m_bloom->init(config));

	m_finalComposite.reset(m_alloc.newInstance<FinalComposite>(this));
	ANKI_CHECK(m_finalComposite->init(config));

	m_dbg.reset(m_alloc.newInstance<Dbg>(this));
	ANKI_CHECK(m_dbg->init(config));

	SamplerInitInfo sinit;
	sinit.m_repeat = false;
	sinit.m_minMagFilter = SamplingFilter::NEAREST;
	m_nearestSampler = m_gr->newInstance<Sampler>(sinit);

	sinit.m_minMagFilter = SamplingFilter::LINEAR;
	m_linearSampler = m_gr->newInstance<Sampler>(sinit);

	initJitteredMats();

	return ErrorCode::NONE;
}

void Renderer::initJitteredMats()
{
	static const Array<Vec2, 16> SAMPLE_LOCS_16 = {{Vec2(-8.0, 0.0),
		Vec2(-6.0, -4.0),
		Vec2(-3.0, -2.0),
		Vec2(-2.0, -6.0),
		Vec2(1.0, -1.0),
		Vec2(2.0, -5.0),
		Vec2(6.0, -7.0),
		Vec2(5.0, -3.0),
		Vec2(4.0, 1.0),
		Vec2(7.0, 4.0),
		Vec2(3.0, 5.0),
		Vec2(0.0, 7.0),
		Vec2(-1.0, 3.0),
		Vec2(-4.0, 6.0),
		Vec2(-7.0, 8.0),
		Vec2(-5.0, 2.0)}};

	for(U i = 0; i < 16; ++i)
	{
		Vec2 texSize(1.0f / Vec2(m_width, m_height)); // Texel size
		texSize *= 2.0f; // Move it to NDC

		Vec2 S = SAMPLE_LOCS_16[i] / 8.0f; // In [-1, 1]

		Vec2 subSample = S * texSize; // In [-texSize, texSize]
		subSample *= 0.5f; // In [-texSize / 2, texSize / 2]

		m_jitteredMats16x[i] = Mat4::getIdentity();
		m_jitteredMats16x[i].setTranslationPart(Vec4(subSample, 0.0, 1.0));
	}

	static const Array<Vec2, 8> SAMPLE_LOCS_8 = {{Vec2(-7.0, 1.0),
		Vec2(-5.0, -5.0),
		Vec2(-1.0, -3.0),
		Vec2(3.0, -7.0),
		Vec2(5.0, -1.0),
		Vec2(7.0, 7.0),
		Vec2(1.0, 3.0),
		Vec2(-3.0, 5.0)}};

	for(U i = 0; i < 8; ++i)
	{
		Vec2 texSize(1.0f / Vec2(m_width, m_height)); // Texel size
		texSize *= 2.0f; // Move it to NDC

		Vec2 S = SAMPLE_LOCS_8[i] / 8.0f; // In [-1, 1]

		Vec2 subSample = S * texSize; // In [-texSize, texSize]
		subSample *= 0.5f; // In [-texSize / 2, texSize / 2]

		m_jitteredMats8x[i] = Mat4::getIdentity();
		m_jitteredMats8x[i].setTranslationPart(Vec4(subSample, 0.0, 1.0));
	}
}

Error Renderer::render(RenderingContext& ctx)
{
	CommandBufferPtr& cmdb = ctx.m_commandBuffer;

	ctx.m_jitterMat = m_jitteredMats8x[m_frameCount & (8 - 1)];
	ctx.m_projMatJitter = ctx.m_jitterMat * ctx.m_projMat;
	ctx.m_viewProjMatJitter = ctx.m_projMatJitter * ctx.m_viewMat;

	ctx.m_prevViewProjMat = m_prevViewProjMat;
	ctx.m_prevCamTransform = m_prevCamTransform;

	// Check if resources got loaded
	if(m_prevLoadRequestCount != m_resources->getLoadingRequestCount()
		|| m_prevAsyncTasksCompleted != m_resources->getAsyncTaskCompletedCount())
	{
		m_prevLoadRequestCount = m_resources->getLoadingRequestCount();
		m_prevAsyncTasksCompleted = m_resources->getAsyncTaskCompletedCount();
		m_resourcesDirty = true;
	}
	else
	{
		m_resourcesDirty = false;
	}

	// Run stages
	m_indirect->run(ctx);

	ANKI_CHECK(m_lightShading->binLights(ctx));

	m_lensFlare->resetOcclusionQueries(ctx, cmdb);
	ANKI_CHECK(buildCommandBuffers(ctx));

	// Barriers
	m_shadowMapping->setPreRunBarriers(ctx);
	m_gbuffer->setPreRunBarriers(ctx);

	// Passes
	m_shadowMapping->run(ctx);
	m_gbuffer->run(ctx);

	// Barriers
	m_gbuffer->setPostRunBarriers(ctx);
	m_shadowMapping->setPostRunBarriers(ctx);
	m_depth->m_hd.setPreRunBarriers(ctx);

	// Passes
	m_depth->m_hd.run(ctx);
	m_lensFlare->updateIndirectInfo(ctx, cmdb);

	// Barriers
	m_depth->m_hd.setPostRunBarriers(ctx);
	m_depth->m_qd.setPreRunBarriers(ctx);

	// Passes
	m_depth->m_qd.run(ctx);

	// Barriers
	m_depth->m_qd.setPostRunBarriers(ctx);
	m_vol->m_main.setPreRunBarriers(ctx);
	m_ssao->m_main.setPreRunBarriers(ctx);

	// Passes
	m_vol->m_main.run(ctx);
	m_ssao->m_main.run(ctx);

	// Barriers
	m_vol->m_main.setPostRunBarriers(ctx);
	m_vol->m_hblur.setPreRunBarriers(ctx);
	m_ssao->m_main.setPostRunBarriers(ctx);
	m_ssao->m_hblur.setPreRunBarriers(ctx);

	// Passes
	m_vol->m_hblur.run(ctx);
	m_ssao->m_hblur.run(ctx);

	// Barriers
	m_vol->m_hblur.setPostRunBarriers(ctx);
	m_vol->m_vblur.setPreRunBarriers(ctx);
	m_ssao->m_hblur.setPostRunBarriers(ctx);
	m_ssao->m_vblur.setPreRunBarriers(ctx);

	// Passes
	m_vol->m_vblur.run(ctx);
	m_ssao->m_vblur.run(ctx);

	// Barriers
	m_vol->m_vblur.setPostRunBarriers(ctx);
	m_ssao->m_vblur.setPostRunBarriers(ctx);
	m_lightShading->setPreRunBarriers(ctx);
	m_forwardShading->setPreRunBarriers(ctx);

	// Passes
	m_lightShading->run(ctx);
	m_forwardShading->run(ctx);

	// Barriers
	m_forwardShading->setPostRunBarriers(ctx);

	// Passes
	m_fsUpscale->run(ctx);

	// Barriers
	cmdb->setTextureSurfaceBarrier(m_lightShading->getRt(),
		TextureUsageBit::FRAMEBUFFER_ATTACHMENT_READ_WRITE,
		TextureUsageBit::SAMPLED_FRAGMENT,
		TextureSurfaceInfo(0, 0, 0, 0));
	m_temporalAA->setPreRunBarriers(ctx);

	// Passes
	m_temporalAA->run(ctx);

	// Barriers
	m_temporalAA->setPostRunBarriers(ctx);
	m_downscale->setPreRunBarriers(ctx);

	// Passes
	m_downscale->run(ctx);

	// Barriers
	m_downscale->setPostRunBarriers(ctx);

	// Passes
	m_tonemapping->run(ctx);

	// Barriers
	m_bloom->m_extractExposure.setPreRunBarriers(ctx);

	// Passes
	m_bloom->m_extractExposure.run(ctx);

	// Barriers
	m_bloom->m_extractExposure.setPostRunBarriers(ctx);
	m_bloom->m_upscale.setPreRunBarriers(ctx);

	// Passes
	m_bloom->m_upscale.run(ctx);

	// Barriers
	m_bloom->m_upscale.setPostRunBarriers(ctx);

	if(m_dbg->getEnabled())
	{
		ANKI_CHECK(m_dbg->run(ctx));
	}

	// Passes
	ANKI_CHECK(m_finalComposite->run(ctx));

	++m_frameCount;
	m_prevViewProjMat = ctx.m_viewProjMat;
	m_prevCamTransform = ctx.m_camTrfMat;

	return ErrorCode::NONE;
}

Vec3 Renderer::unproject(
	const Vec3& windowCoords, const Mat4& modelViewMat, const Mat4& projectionMat, const int view[4])
{
	Mat4 invPm = projectionMat * modelViewMat;
	invPm.invert();

	// the vec is in NDC space meaning: -1<=vec.x<=1 -1<=vec.y<=1 -1<=vec.z<=1
	Vec4 vec;
	vec.x() = (2.0 * (windowCoords.x() - view[0])) / view[2] - 1.0;
	vec.y() = (2.0 * (windowCoords.y() - view[1])) / view[3] - 1.0;
	vec.z() = 2.0 * windowCoords.z() - 1.0;
	vec.w() = 1.0;

	Vec4 out = invPm * vec;
	out /= out.w();
	return out.xyz();
}

TextureInitInfo Renderer::create2DRenderTargetInitInfo(
	U32 w, U32 h, const PixelFormat& format, TextureUsageBit usage, SamplingFilter filter, U mipsCount, CString name)
{
	ANKI_ASSERT(!!(usage & TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE));
	TextureInitInfo init(name);

	init.m_width = w;
	init.m_height = h;
	init.m_depth = 1;
	init.m_layerCount = 1;
	init.m_type = TextureType::_2D;
	init.m_format = format;
	init.m_mipmapsCount = mipsCount;
	init.m_samples = 1;
	init.m_usage = usage;
	init.m_sampling.m_minMagFilter = filter;
	if(mipsCount > 1)
	{
		init.m_sampling.m_mipmapFilter = filter;
	}
	else
	{
		init.m_sampling.m_mipmapFilter = SamplingFilter::BASE;
	}
	init.m_sampling.m_repeat = false;
	init.m_sampling.m_anisotropyLevel = 0;

	return init;
}

TexturePtr Renderer::createAndClearRenderTarget(const TextureInitInfo& inf)
{
	ANKI_ASSERT(!!(inf.m_usage & TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE));

	const U faceCount = (inf.m_type == TextureType::CUBE || inf.m_type == TextureType::CUBE_ARRAY) ? 6 : 1;

	// Create tex
	TexturePtr tex = m_gr->newInstance<Texture>(inf);

	// Clear all surfaces
	CommandBufferInitInfo cmdbinit;
	cmdbinit.m_flags = CommandBufferFlag::GRAPHICS_WORK;
	if((inf.m_mipmapsCount * faceCount * inf.m_layerCount * 4) < COMMAND_BUFFER_SMALL_BATCH_MAX_COMMANDS)
	{
		cmdbinit.m_flags |= CommandBufferFlag::SMALL_BATCH;
	}
	CommandBufferPtr cmdb = m_gr->newInstance<CommandBuffer>(cmdbinit);

	for(U mip = 0; mip < inf.m_mipmapsCount; ++mip)
	{
		for(U face = 0; face < faceCount; ++face)
		{
			for(U layer = 0; layer < inf.m_layerCount; ++layer)
			{
				TextureSurfaceInfo surf(mip, 0, face, layer);

				FramebufferInitInfo fbInit;

				if(inf.m_format.m_components >= ComponentFormat::FIRST_DEPTH_STENCIL
					&& inf.m_format.m_components <= ComponentFormat::LAST_DEPTH_STENCIL)
				{
					fbInit.m_depthStencilAttachment.m_texture = tex;
					fbInit.m_depthStencilAttachment.m_surface = surf;
					fbInit.m_depthStencilAttachment.m_aspect = DepthStencilAspectBit::DEPTH_STENCIL;
					fbInit.m_depthStencilAttachment.m_loadOperation = AttachmentLoadOperation::CLEAR;
				}
				else
				{
					fbInit.m_colorAttachmentCount = 1;
					fbInit.m_colorAttachments[0].m_texture = tex;
					fbInit.m_colorAttachments[0].m_surface = surf;
					fbInit.m_colorAttachments[0].m_loadOperation = AttachmentLoadOperation::CLEAR;
					fbInit.m_colorAttachments[0].m_stencilLoadOperation = AttachmentLoadOperation::CLEAR;
				}
				FramebufferPtr fb = m_gr->newInstance<Framebuffer>(fbInit);

				cmdb->setTextureSurfaceBarrier(
					tex, TextureUsageBit::NONE, TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE, surf);

				cmdb->beginRenderPass(fb);
				cmdb->endRenderPass();

				if(!!inf.m_initialUsage)
				{
					cmdb->setTextureSurfaceBarrier(
						tex, TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE, inf.m_initialUsage, surf);
				}
			}
		}
	}

	cmdb->flush();

	return tex;
}

void Renderer::buildCommandBuffersInternal(RenderingContext& ctx, U32 threadId, PtrSize threadCount)
{
	// MS
	//
	m_gbuffer->buildCommandBuffers(ctx, threadId, threadCount);

	// Append to the last MS's cmdb the occlusion tests
	if(ctx.m_gbuffer.m_lastThreadWithWork == threadId)
	{
		m_lensFlare->runOcclusionTests(ctx, ctx.m_gbuffer.m_commandBuffers[threadId]);
	}

	if(ctx.m_gbuffer.m_commandBuffers[threadId])
	{
		ctx.m_gbuffer.m_commandBuffers[threadId]->flush();
	}

	// SM
	//
	m_shadowMapping->buildCommandBuffers(ctx, threadId, threadCount);

	// FS
	//
	m_forwardShading->buildCommandBuffers(ctx, threadId, threadCount);

	// Append to the last FB's cmdb the other passes
	if(ctx.m_forwardShading.m_lastThreadWithWork == threadId)
	{
		m_lensFlare->run(ctx, ctx.m_forwardShading.m_commandBuffers[threadId]);
		m_forwardShading->drawVolumetric(ctx, ctx.m_forwardShading.m_commandBuffers[threadId]);
	}
	else if(threadId == threadCount - 1 && ctx.m_forwardShading.m_lastThreadWithWork == MAX_U32)
	{
		// There is no FS work. Create a cmdb just for LF & VOL

		CommandBufferInitInfo init;
		init.m_flags =
			CommandBufferFlag::GRAPHICS_WORK | CommandBufferFlag::SECOND_LEVEL | CommandBufferFlag::SMALL_BATCH;
		init.m_framebuffer = m_forwardShading->getFramebuffer();
		CommandBufferPtr cmdb = getGrManager().newInstance<CommandBuffer>(init);

		// Inform on textures
		cmdb->informTextureSurfaceCurrentUsage(m_forwardShading->getRt(),
			TextureSurfaceInfo(0, 0, 0, 0),
			TextureUsageBit::FRAMEBUFFER_ATTACHMENT_READ_WRITE);
		cmdb->informTextureSurfaceCurrentUsage(m_depth->m_hd.m_depthRt,
			TextureSurfaceInfo(0, 0, 0, 0),
			TextureUsageBit::FRAMEBUFFER_ATTACHMENT_READ | TextureUsageBit::SAMPLED_FRAGMENT);

		cmdb->setViewport(0, 0, m_forwardShading->getWidth(), m_forwardShading->getHeight());

		m_lensFlare->run(ctx, cmdb);
		m_forwardShading->drawVolumetric(ctx, cmdb);

		ctx.m_forwardShading.m_commandBuffers[threadId] = cmdb;
	}

	if(ctx.m_forwardShading.m_commandBuffers[threadId])
	{
		ctx.m_forwardShading.m_commandBuffers[threadId]->flush();
	}
}

Error Renderer::buildCommandBuffers(RenderingContext& ctx)
{
	ANKI_TRACE_SCOPED_EVENT(RENDERER_COMMAND_BUFFER_BUILDING);
	ThreadPool& threadPool = getThreadPool();

	// Prepare
	if(m_shadowMapping)
	{
		m_shadowMapping->prepareBuildCommandBuffers(ctx);
	}

	// Find the last jobs for MS and FS
	U32 lastMsJob = MAX_U32;
	U32 lastFsJob = MAX_U32;
	U threadCount = threadPool.getThreadsCount();
	for(U i = threadCount - 1; i != 0; --i)
	{
		if(threadWillDoWork(ctx, VisibilityGroupType::RENDERABLES_MS, i, threadCount) && lastMsJob == MAX_U32)
		{
			lastMsJob = i;
		}

		if(threadWillDoWork(ctx, VisibilityGroupType::RENDERABLES_FS, i, threadCount) && lastFsJob == MAX_U32)
		{
			lastFsJob = i;
		}
	}

	ctx.m_gbuffer.m_lastThreadWithWork = lastMsJob;
	ctx.m_forwardShading.m_lastThreadWithWork = lastFsJob;

	// Build
	class Task : public ThreadPoolTask
	{
	public:
		Renderer* m_r ANKI_DBG_NULLIFY;
		RenderingContext* m_ctx ANKI_DBG_NULLIFY;

		Error operator()(U32 threadId, PtrSize threadCount)
		{
			m_r->buildCommandBuffersInternal(*m_ctx, threadId, threadCount);
			return ErrorCode::NONE;
		}
	};

	Task task;
	task.m_r = this;
	task.m_ctx = &ctx;
	for(U i = 0; i < threadPool.getThreadsCount(); i++)
	{
		threadPool.assignNewTask(i, &task);
	}

	ANKI_CHECK(threadPool.waitForAllThreadsToFinish());

	return ErrorCode::NONE;
}

Error Renderer::createShader(CString fname, ShaderResourcePtr& shader, CString extra)
{
	return m_resources->loadResourceToCache(shader, fname, &extra[0], "r_");
}

Error Renderer::createShaderf(CString fname, ShaderResourcePtr& shader, CString fmt, ...)
{
	Array<char, 512> buffer;
	va_list args;

	va_start(args, fmt);
	I len = std::vsnprintf(&buffer[0], sizeof(buffer), &fmt[0], args);
	va_end(args);
	ANKI_ASSERT(len > 0 && len < I(sizeof(buffer) - 1));
	(void)len;

	return m_resources->loadResourceToCache(shader, fname, &buffer[0], "r_");
}

void Renderer::createDrawQuadShaderProgram(ShaderPtr frag, ShaderProgramPtr& prog)
{
	prog = m_gr->newInstance<ShaderProgram>(m_drawQuadVert->getGrShader(), frag);
}

} // end namespace anki
