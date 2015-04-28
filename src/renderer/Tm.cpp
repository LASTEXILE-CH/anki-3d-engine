// Copyright (C) 2009-2015, Panagiotis Christopoulos Charitos.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include "anki/renderer/Tm.h"
#include "anki/renderer/Is.h"
#include "anki/renderer/Renderer.h"

namespace anki {

//==============================================================================
Error Tm::create(const ConfigSet& initializer)
{
	// Create shader
	StringAuto pps(getAllocator());

	pps.sprintf(
		"#define IS_RT_MIPMAP %u\n"
		"#define ANKI_RENDERER_WIDTH %u\n"
		"#define ANKI_RENDERER_HEIGHT %u\n",
		Is::MIPMAPS_COUNT,
		m_r->getWidth(),
		m_r->getHeight());

	ANKI_CHECK(m_luminanceShader.loadToCache(&getResourceManager(), 
		"shaders/PpsTmAverageLuminance.comp.glsl", pps.toCString(), "rppstm_"));

	// Create ppline
	PipelineHandle::Initializer pplineInit;
	pplineInit.m_shaders[U(ShaderType::COMPUTE)] = 
		m_luminanceShader->getGrShader();
	ANKI_CHECK(m_luminancePpline.create(&getGrManager(), pplineInit));

	// Create buffer
	ANKI_CHECK(m_luminanceBuff.create(&getGrManager(), GL_SHADER_STORAGE_BUFFER,
		nullptr, sizeof(Vec4), GL_DYNAMIC_STORAGE_BIT));

	return ErrorCode::NONE;
}

//==============================================================================
void Tm::run(CommandBufferHandle& cmdb)
{
	m_luminancePpline.bind(cmdb);
	m_luminanceBuff.bindShaderBuffer(cmdb, 0);
	m_r->getIs()._getRt().bind(cmdb, 0);

	cmdb.dispatchCompute(1, 1, 1);
}

} // end namespace anki
