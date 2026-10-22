#pragma once

#include "util/types.hpp"
#include "Emu/Io/midi_config_types.h"

#include <mutex>
#include <thread>

#include <QObject>
#include <QStringList>

class midi_creator : public QObject
{
	Q_OBJECT

public:
	midi_creator();
	virtual ~midi_creator();

	QString get_none() const;
	std::string set_device(u32 num, const midi_device& device);
	void parse_devices(std::string_view list);
	void refresh_list();
	const QStringList& get_midi_list() const;
	const std::array<midi_device, max_midi_devices>& get_selection_list() const;

private:
	// The async rtmidi warm-up.  A joinable std::thread that is destroyed without a
	// join aborts the process, and the exit paths that call exit() directly (AppKit's
	// terminate, which runs no Qt destructor) join nothing, so the thread object is
	// joined at static destruction as well.
	struct init_thread_guard
	{
		~init_thread_guard();
	};

	static std::mutex m_midi_init_mutex;
	static std::unique_ptr<std::thread> m_midi_init_thread;
	static init_thread_guard m_midi_init_thread_guard;

	static void join_init_thread();

	QStringList m_midi_list;
	std::array<midi_device, max_midi_devices> m_sel_list;
};
