#pragma once

#include "MTLDevice.h"
#include "MTLResourceManager.h"
#include "MTLFormats.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLArgumentBinds.h"
#include "Emu/RSX/Utils/rsx_utils.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mtl
{
	class command_recorder;

	// The commit feedback blocks outlive the recorder: Metal keeps a block until
	// the queue delivers its feedback, which can be after shutdown.  The block
	// therefore captures this owner by value and reaches the recorder only
	// through the pointer it guards (L3).
	struct feedback_owner
	{
		std::mutex lock;
		command_recorder* recorder = nullptr;
		std::atomic<u32> in_flight{0};
	};

	// One frame slot: one allocator, a small pool of command buffers, and one
	// argument table per stage (HLD-04 §3.1).  A mid-frame commit (read-back,
	// label release) ends the open buffer and continues in the next one, so the
	// allocator is reset only when the slot is recycled, never while its buffers
	// may still run (A1/R1).
	struct frame_slot
	{
		static constexpr u32 buffer_capacity = 8;

		MTL4::CommandAllocator* allocator = nullptr;
		std::array<MTL4::CommandBuffer*, buffer_capacity> buffers{};
		// The object the next buffer uses.  Every entry below it has been
		// allocated at least once; a reset reuses them from zero.
		u32 buffer_index = 0;
		MTL4::ArgumentTable* vertex_table = nullptr;
		MTL4::ArgumentTable* fragment_table = nullptr;
		// The interpreter draws from its own tables so its compact texture and
		// sampler bindings never collide with a regular draw's bindings in the
		// shared tables (LLD-shader-interpreter §4.2, §7.1).
		MTL4::ArgumentTable* interpreter_vertex_table = nullptr;
		MTL4::ArgumentTable* interpreter_fragment_table = nullptr;
		// The blit pass binds a source texture, its sampler and its values through a
		// table of its own.  argument_binder only tracks its own writes, so a table
		// shared with a draw would let a stale bind of the blit's sampler survive
		// into the next draw's slot (MTLArgumentBinds.h, argument_binder).
		MTL4::ArgumentTable* blit_table = nullptr;
		// The completion-event value this slot's last commit reaches when the GPU
		// has finished it.  Zero means the slot is idle.  The queue signals in
		// commit order, so one monotone value covers every slot (SY14).  Only the
		// RSX thread writes or reads it; the event's own value is the cross-thread
		// signal the wait polls.
		u64 completion_value = 0;
		// The commit generation of the slot.  A late feedback carries the generation
		// it belongs to, so a feedback of an abandoned commit cannot clear a slot
		// that a newer commit now owns.  The commit path writes it and the feedback
		// handler reads it on another thread, so it is atomic.
		std::atomic<u32> generation{0};
		// The bytes of the slot's label-staging region that its command buffers
		// already reference.  The region is reset only when no command buffer of the
		// slot can still run, because the GPU reads the staged words at execution
		// time, not at encode time (HLD-04 §4 invariant 2).
		u32 label_staging_offset = 0;
		// True when the slot's command buffer has been begun since its last end.
		bool buffer_open = false;
		// The number of label addresses in `labels` that belong to a command
		// buffer already committed.  The rest are still staged or encoded in the
		// open buffer, so an acquire on one must commit it first (A1/R1).
		u32 committed_label_count = 0;
		// The addresses whose label writes are in this slot's command buffer (or
		// its last commit, until that commit reports back).  A guest acquire on one
		// of them must wait for this slot before it reads the old guest word.
		std::vector<u32> labels;
		// TODO(excise): level 4; the CPU commit time used by the feedback timings.
		// It is written on the RSX thread and read on the driver's feedback
		// callback thread, so it is atomic like the generation beside it.
		std::atomic<f64> debug_commit_time{0.0};
	};

	// The command recorder (HLD-01 C4, HLD-04 §3.1).  It is the single chokepoint
	// for the frame's encoder stream: one open encoder at a time, one way to open a
	// pass, one way to submit.  Encoding stays on the RSX thread (HLD-04 §2.3).
	class command_recorder
	{
	public:
		void init(device& dev, resource_manager& resources);
		// False when the frame ring did not drain.  The caller then leaks the
		// backend rather than freeing objects the GPU may still read (L2).
		bool shutdown();

		// True once a commit timed out or a feedback reported an error.  The core
		// stops issuing draws and the UI can report the loss (L1, L3).
		bool device_lost() const
		{
			return m_device_lost;
		}

		// Wait for the slot to come back from the GPU, recycle its frame resources,
		// then open the slot's command buffer (HLD-04 §3.2).  Returns false when the
		// previous commit of this slot did not report its feedback: the slot then
		// holds fresh objects and its previous work is abandoned, so a consumer that
		// tracks the slot must not treat that work as complete (Q3).
		bool begin_frame();
		void submit();

		// The number of commits of `slot` whose feedback has been delivered.  The
		// count advances in commit order on one queue, so a later commit proves
		// every earlier commit of the same slot complete.  The query gate uses it so
		// a result is readable in the frame that produced it (Q4).
		u64 completed_commits(u32 slot) const;

		// Wait for every committed slot to report its feedback.  A CPU-visible
		// memory write outside a frame uses this before it publishes the value.
		bool wait_idle();

		// A CPU readback must observe the copy it encoded, and a Metal 4 command
		// buffer is the unit of submission, so the frame commits early and waits for
		// the slot to come back.  The slot is re-entered, because the frame owns
		// exactly one slot, and the present drawable is left for the frame's own
		// submit (REVIEW-FINDINGS.md, B4).
		void flush_for_readback();

		// Write `value` (the guest word, in memory order) to `address` on the GPU, at
		// the current point in the command stream.  The write is recorded, not
		// submitted, so the caller never waits for the GPU to reach it.  False when
		// the address is not in the guest window; the caller then stores it on the
		// CPU after a drain.  A label is the only signal the GPU gives the guest, so
		// a CPU store cannot be ordered against the work it releases (L130).
		bool write_label(u32 address, u32 value);

		// True while a label write is recorded in a command buffer that has not been
		// submitted: a guest that waits for it would wait for ever.
		bool label_writes_pending() const { return m_pending_count != 0; }

		// Ask for the pending label writes to be committed at the next natural
		// boundary rather than immediately, so a release inside a pass does not
		// split it (A2).  The next pass boundary commits; the caller also checks
		// `label_commit_due` against its deadline for a parked FIFO.
		void request_label_commit()
		{
			m_label_commit_due = true;
		}
		bool label_commit_due() const
		{
			return m_label_commit_due;
		}

		// Submit the frame's recorded work so the pending label writes become
		// visible, and leave the slot to be re-entered by the next command.  A guest
		// waiting on a label needs this; it does not wait for the GPU, the guest's
		// own wait is the wait (HLD-04 §4.3).
		void commit_pending_labels();

		// A semaphore acquire read the pre-release word, and a label write to
		// `address` is still recorded or in flight.  Commit the batch and wait for
		// the slot that carries it, then return true so the caller re-reads the
		// word.  False when no write to this address is pending, which is the
		// common case and costs one scan of the per-slot address lists.
		bool sync_label(u32 address);

		MTL4::RenderCommandEncoder* open_pass(const MTL4::RenderPassDescriptor* descriptor, MTL4::RenderEncoderOptions options = MTL4::RenderEncoderOptionNone);
		MTL4::ComputeCommandEncoder* open_compute();

		// Byte-swap `size` bytes of `buffer` at `offset` in place, one guest element
		// of `element_size` (2 or 4) at a time.  The copies around it are blits in
		// the same compute encoder, and Metal 4 tracks no hazards, so the dispatch
		// is fenced on both sides with an encoder-scope edge (T2).
		void swap_bytes(MTL4::ComputeCommandEncoder* encoder, MTL::Buffer* buffer, u64 offset, u64 size, u32 element_size);

		// One side of a typeless copy.  `depth` names the guest depth encoding,
		// because a D32F_S8 image cannot tell z24s8_uint from z24s8_float (T6).
		struct typeless_region
		{
			MTL::Texture* image = nullptr;
			areai rect{};
			u32 level = 0;
			u32 slice = 0;
			bool depth = false;
			rsx::surface_depth_format2 depth_format = rsx::surface_depth_format2::z24s8_uint;
		};

		// Copy `src` into `dst` when the two images describe the same guest bytes
		// under different formats: same format is a texture copy, two colour
		// formats stage through a buffer, and depth ↔ colour is a compute
		// conversion.  Returns false when no bit-preserving route exists (T6).
		bool copy_typeless(command_recorder& cmd, const typeless_region& src, const typeless_region& dst);

		// Resolve a multisampled colour image into the guest sample mosaic (T3).
		// The mosaic is width*samples_x by height*samples_y; the compute kernel
		// scatters each guest sample to its slot, so the guest bytes stay canonical.
		void resolve_color(MTL::Texture* multisampled, MTL::Texture* mosaic);

		// Resolve depth (and stencil, for a combined format) into the mosaic.  The
		// multisampled stencil view is read as a uint texture; `combined` adds the
		// stencil attachment and the [[stencil]] output.
		void resolve_depth_stencil(MTL::Texture* multisampled, MTL::Texture* mosaic,
			MTL::Texture* multisampled_stencil, u8 grid_x, u8 grid_y, bool combined);

		// Scatter the mosaic back into the multisampled colour image.  A compute
		// kernel cannot write a multisample texture, so this is a per-sample pass.
		void unresolve_color(MTL::Texture* multisampled, MTL::Texture* mosaic,
			u8 host_samples, u8 grid_x, u8 grid_y);

		// Scatter the mosaic depth (and stencil) back into the multisampled image.
		void unresolve_depth_stencil(MTL::Texture* multisampled, MTL::Texture* mosaic,
			MTL::Texture* mosaic_stencil, u8 host_samples, u8 grid_x, u8 grid_y, bool combined);

		// Average the mosaic over the guest grid into a pixel-sized image.  The
		// present path uses it to keep the hardware-average look without a
		// pixel-sized resolve target in the surface itself (T3).
		void average_mosaic(MTL::Texture* mosaic, MTL::Texture* output);

		// End the open encoder, if any.  Every encoder ends with a producer-scope
		// ALL->ALL barrier: Metal 4 tracks no hazards, so the encoder boundary is
		// the one place the backend can guarantee that a later encoder observes
		// everything an earlier one wrote.  This is the recorder's contract, not the
		// scheduler's; the scheduler is an optimiser and may not weaken it
		// (METAL4-ARCH-REDESIGN-2026-11-09.md C2).
		void close_encoder();

		// The encoder currently open on the recorder, or null.  A caller that keeps
		// an encoder across draws must compare its own pointer with this: a copy,
		// upload, readback or re-entered slot closes the encoder behind its back.
		MTL4::CommandEncoder* current_encoder() const { return m_encoder; }

		// EXCISE-BEGIN encoder-provenance
		// TODO(excise): level 1; the encoder state machine's transitions.  A fault
		// inside the driver on setArgumentTable is an encoder that was already
		// ended, so a caller compares the stamp it captured with this one and, on a
		// difference, asks for the history: it names the transition that ended the
		// encoder and the site that asked for it.
		u32 encoder_stamp() const { return m_encoder_stamp; }
		void dump_encoder_events(const char* why) const;
		// EXCISE-END encoder-provenance

		// EXCISE-BEGIN encoder-watch
		// TODO(excise): level 1; the encoder the draw is encoding into.  If the
		// recorder closes it while the draw still holds it, that close *is* the
		// defect, so the trap takes the stack at the close rather than inferring it
		// from a pointer that is already stale.  One scope per draw, four reports.
		void watch_encoder(MTL4::CommandEncoder* encoder, u64 draw);
		void dump_stack(const char* why, void* const* frames, u32 count) const;
		bool buffer_open() const { return m_slots[m_slot].buffer_open; }
		// EXCISE-END encoder-watch

		// The lifetime of the argument table objects.  A re-entered slot that had to
		// take fresh objects has new tables, and a writer that caches what it wrote
		// into the old ones must start over: an unchanged slot would otherwise stay
		// unbound and read null.  The generation changes only when a table is
		// replaced, never on the ordinary slot rotation (which resets the writers
		// at the frame boundary).
		u32 table_generation() const { return m_table_generation; }

		MTL4::ArgumentTable* vertex_table() const { return m_slots[m_slot].vertex_table; }
		MTL4::ArgumentTable* fragment_table() const { return m_slots[m_slot].fragment_table; }
		MTL4::ArgumentTable* interpreter_vertex_table() const { return m_slots[m_slot].interpreter_vertex_table; }
		MTL4::ArgumentTable* interpreter_fragment_table() const { return m_slots[m_slot].interpreter_fragment_table; }
		MTL4::ArgumentTable* blit_table() const { return m_slots[m_slot].blit_table; }

		// The open slot, so the per-frame rings size and reset with it.
		u32 slot() const { return m_slot; }

		// The drawable this frame presents (C7).  The recorder waits for it before
		// it encodes and signals it after the commit, so a present stays on the one
		// submit path (HLD-03 §4.9).  Null drops the present.
		void set_present_drawable(MTL::Drawable* drawable);

		// The resource layer, for the traits that allocate a surface.
		resource_manager& resources() const { return *m_resources; }

	private:
		// A label batch is staged per frame slot; the GPU reads the staged words when
		// the command buffer runs, so a region is only rewritten once its slot has
		// completed every commit that references it.
		static constexpr u32 label_staging_capacity = 4096;
		static constexpr u32 label_staging_bytes = label_staging_capacity * sizeof(u32);

		void release_slot(frame_slot& slot);
		void allocate_slot(frame_slot& slot);
		u64 commit_slot(frame_slot& slot, u32 index, u32 generation, MTL4::CommandBuffer* buffer);
		void on_feedback(u32 index, u32 generation, MTL4::CommitFeedback* feedback);

		// True when the slot's last commit has reached the GPU.  One atomic load,
		// so a wait that is already satisfied costs the same as a test (SY14).
		bool slot_idle(const frame_slot& slot) const;
		bool ring_idle() const;

		// Poll the completion event until `value` is reached.  False on timeout.
		// The feedback handler is not on this path: the event is the GPU's own
		// signal, and a condition variable would add a lock and a wake-up to a
		// check that is almost always already true.
		bool await_value(u64 value, const char* what);
		bool await_slot(const frame_slot& slot, const char* what);

		// Begin the slot's next command buffer.  This never waits: the allocator
		// keeps handing out buffers until the slot's pool is full, and only then
		// does the slot await its own completion and reset (A1/R1).
		bool next_buffer();

		// End the open command buffer, commit it and return the completion value
		// the queue will signal.  Zero when nothing was open.  The caller decides
		// whether to await the value; a label commit does not.
		u64 commit_open_buffer(const char* what);

		// Wait for the slot's last commit and reset its allocator and buffer pool.
		// Called when the slot is recycled at the frame boundary.
		bool recycle_slot(const char* what);

		// Latch device loss.  Recording stops; nothing in flight is freed.
		void mark_device_lost(const char* why);

		// Open the compute encoder that carries copies, closing an open pass.  A
		// consecutive copy reuses it, so a batch costs one encoder (HLD-04 §3.1).
		MTL4::ComputeCommandEncoder* open_copy_encoder();

		// Emit the pending label writes into one compute encoder.  One queue barrier
		// orders the whole batch after the work encoded ahead of it.
		void flush_label_writes();

		// Publish the allocations the resource manager made persistent since the last
		// commit.  Residency additions are batched, and this is the one place that
		// has to make sure they are in the set before the buffer is committed (P6).
		void publish_residency();

		// Build the guest-endian byte-swap kernels (T2).  They live with the
		// recorder because every upload and read-back path needs them, and the
		// recorder already owns the compiler and the shared blit argument table.
		void build_swap_pipelines();

		// Build the depth ↔ z24s8 conversion kernels (T6).
		void build_convert_pipelines();

		// Build the MSAA resolve/unresolve kernels and pipelines (T3).
		void build_resolve_pipelines();

		// The resolve pass needs the guest grid, which is not derivable from the
		// render target alone, so it travels in a small constant buffer.
		struct resolve_params
		{
			u32 grid_x;
			u32 grid_y;
			u32 pad0;
			u32 pad1;
		};

		enum class resolve_kind : u32
		{
			resolve_color,
			present_average,
			unresolve_color,
			resolve_depth,
			resolve_depth_stencil,
			unresolve_depth,
			unresolve_depth_stencil,
		};

		MTL::RenderPipelineState* resolve_render_pipeline(resolve_kind kind, MTL::PixelFormat format, u8 samples);
		MTL::ComputePipelineState* resolve_compute_pipeline(resolve_kind kind);

		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		MTL4::Compiler* m_compiler = nullptr;
		resource_manager* m_resources = nullptr;
		MTL::Library* m_swap_library = nullptr;
		MTL::ComputePipelineState* m_bswap16 = nullptr;
		MTL::ComputePipelineState* m_bswap32 = nullptr;
		MTL::Library* m_convert_library = nullptr;
		MTL::ComputePipelineState* m_depth_to_pack = nullptr;
		MTL::ComputePipelineState* m_pack_to_depth = nullptr;
		MTL::Library* m_resolve_library = nullptr;
		MTL::ComputePipelineState* m_resolve_color_state = nullptr;
		MTL::ComputePipelineState* m_present_average_state = nullptr;
		MTL::DepthStencilState* m_resolve_depth_state = nullptr;
		std::unordered_map<u64, MTL::RenderPipelineState*> m_resolve_pipelines;
		std::array<frame_slot, frame_slot_count> m_slots{};
		// Delivered commit feedback per slot.  The feedback thread writes it and the
		// RSX thread reads it, so it is atomic.
		std::array<std::atomic<u64>, frame_slot_count> m_completed{};
		// The GPU completion timeline.  Every commit takes the next value and the
		// queue signals it once that commit's work has run, so a slot is complete
		// when `signaledValue` reaches its value (metal-cpp MTL4CommandQueue.hpp:142;
		// KK signals the same way in kk_queue.c:230).
		MTL::SharedEvent* m_completion_event = nullptr;
		u64 m_completion_sequence = 0;
		MTL::Buffer* m_label_staging = nullptr;
		// TODO(excise): level 2; one word per slot, the GPU's own copy of the last
		// label it wrote.  It separates "the copy did not land" from "the guest
		// consumed the word".
		MTL::Buffer* m_label_probe = nullptr;
		std::array<u32, label_staging_capacity> m_pending_labels{};
		u32 m_pending_count = 0;
		// A back-end release asked for the labels to be committed at the next pass
		// boundary instead of splitting the open pass (A2).
		bool m_label_commit_due = false;
		// EXCISE-BEGIN label-verify
		// TODO(excise): level 2; the last GPU label write, verified after feedback.
		u32 m_diag_label_address = 0;
		u32 m_diag_label_value = 0;
		// EXCISE-END label-verify
		u32 m_slot = 0;
		// Bumped by allocate_slot() when a slot has to take fresh argument tables.
		// See table_generation().
		u32 m_table_generation = 0;
		MTL4::CommandEncoder* m_encoder = nullptr;
		MTL4::ComputeCommandEncoder* m_compute_encoder = nullptr;

		// EXCISE-BEGIN encoder-provenance-state
		// TODO(excise): level 1; see encoder_stamp().  The stamp counts every
		// transition; the ring keeps the recent ones, so the history is bounded
		// whether or not it is ever printed.
		enum class encoder_event : u32
		{
			begin_frame, open_pass, open_compute, close_encoder,
			reenter_slot, allocate_slot, release_slot, submit, readback,
		};

		struct encoder_event_record
		{
			u32 stamp = 0;
			encoder_event event = encoder_event::begin_frame;
			MTL4::CommandEncoder* encoder = nullptr;
			void* caller = nullptr;
			u32 slot = 0;
		};

		static constexpr u32 encoder_event_capacity = 24;
		encoder_event_record m_encoder_events[encoder_event_capacity] = {};
		u32 m_encoder_event_count = 0;
		u32 m_encoder_stamp = 0;

		// The jump table in the class body needs both types.
		static const char* encoder_event_name(encoder_event event);

		// A transition.  The caller is the return address of the transition's site,
		// which is what identifies the path that ended an encoder.  The ring is only
		// written when the probe asked for it; the stamp always advances.
		void note_encoder_event(encoder_event event, MTL4::CommandEncoder* encoder, void* caller);
		// EXCISE-END encoder-provenance-state

		// EXCISE-BEGIN encoder-watch-state
		// TODO(excise): level 1; see watch_encoder().
		static constexpr u32 encoder_watch_depth = 24;
		MTL4::CommandEncoder* m_watched_encoder = nullptr;
		u64 m_watched_draw = 0;
		u32 m_watch_reports = 0;
		u32 m_watch_depth = 0;
		void* m_watch_stack[encoder_watch_depth] = {};
		// EXCISE-END encoder-watch-state

		// Set when the slot could not be recycled: every commit of the slot is
		// abandoned and the slot holds fresh objects.  The next command uses them
		// instead of waiting for a completion that will never arrive (L1).
		std::atomic<bool> m_device_lost{false};
		// The device's own loss flag, so check_device_status() sees the same state.
		std::atomic<bool>* m_lost = nullptr; // Feedback blocks capture this owner, not `this` (L3).
		std::shared_ptr<feedback_owner> m_feedback;
		MTL::Drawable* m_drawable = nullptr;
	};
}

#endif // RSX_ENABLE_METAL4
