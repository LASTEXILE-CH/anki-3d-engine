// Copyright (C) 2009-2019, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <anki/Ui.h>
#include <anki/util/List.h>

namespace anki
{

/// @addtogroup core
/// @{

/// Developer console UI.
class DeveloperConsole : public UiImmediateModeBuilder
{
public:
	DeveloperConsole(UiManager* ui)
		: UiImmediateModeBuilder(ui)
	{
	}

	~DeveloperConsole();

	ANKI_USE_RESULT Error init(AllocAlignedCallback allocCb, void* allocCbUserData);

	void build(CanvasPtr ctx) override;

private:
	static constexpr U MAX_LOG_ITEMS = 64;

	class LogItem : public IntrusiveListEnabled<LogItem>
	{
	public:
		const char* m_file;
		const char* m_func;
		const char* m_subsystem;
		String m_msg;
		I32 m_line;
		LoggerMessageType m_type;
	};

	HeapAllocator<U8> m_alloc;
	FontPtr m_font;
	IntrusiveList<LogItem> m_logItems;
	U32 m_logItemCount = 0;
	Array<char, 256> m_inputText;

	void newLogItem(const LoggerMessageInfo& inf);

	static void loggerCallback(void* userData, const LoggerMessageInfo& info)
	{
		static_cast<DeveloperConsole*>(userData)->newLogItem(info);
	}
};
/// @}

} // end namespace anki
