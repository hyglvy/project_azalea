/// @file
/// @brief Supports multi-processor operations.
///
/// Supports multi-processor operations.
/// Allows:
/// - Processors to be enumerated and identified
/// - Processors to be started and stopped
/// - Signals to be sent between processors.
///
/// Functions in this file that do not contain _x64 in their name would be generic to all platforms, but the exact
/// implementation is platform specific.

//#define ENABLE_TRACING

#include "klib/klib.h"
#include "processor/processor.h"
#include "processor/processor-int.h"
#include "processor/x64/processor-x64.h"
#include "processor/x64/processor-x64-int.h"
#include "processor/x64/pic/pic.h"
#include "processor/x64/pic/apic.h"
#include "acpi/acpi_if.h"
#include "mem/x64/mem-x64.h"
#include "syscall/x64/syscall_kernel-x64.h"
#include "processor/timing/timing.h"

/// @brief Controls communication between source and target processors.
enum class PROC_MP_X64_MSG_STATE
{
  /// The default is this state. If a target processor receives an NMI and this is the state then it wasn't generated
  /// by the kernel to signal messages. Once the source processor receives its acknowledgement it should set this
  /// value again.
  NO_MSG,

  /// Tells the target processor that a message is waiting for it.
  MSG_WAITING,

  /// The target processor sets this value after dealing with its IPI in order to let the source know it has done its
  /// work.
  ACKNOWLEDGED,
};

/// A structure for storing details of inter-processor communications
struct proc_mp_ipi_msg_state
{
  /// The message sent by the initiator of communication.
  PROC_IPI_MSGS msg_being_sent;

  /// The current state of the communication. See the documentation of PROC_MP_X64_MSG_STATE for more details.
  volatile PROC_MP_X64_MSG_STATE msg_control_state;

  /// Prevents more than one processor signalling the target at once. Controlled by the initiator
  kernel_spinlock signal_lock;
};

const uint8_t SUBTABLE_LAPIC_TYPE = 0;

static proc_mp_ipi_msg_state *inter_proc_signals = nullptr;

extern "C" uint64_t asm_ap_trampoline_start;
extern "C" uint64_t asm_ap_trampoline_end;
extern "C" uint64_t asm_ap_trampoline_addr;

/// @brief Prepare the system to start multi-processing
///
/// Counts up the others processors and gathers useful information, but doesn't signal them to start just yet.
void proc_mp_init()
{
  KL_TRC_ENTRY;

  ACPI_STATUS retval;
  char table_name[] = "APIC";
  acpi_table_madt *madt_table;
  acpi_subtable_header *subtable;
  acpi_madt_local_apic *lapic_table;
  uint32_t procs_saved = 0;
  uint32_t trampoline_length;
  uint64_t start_time;
  uint64_t wait_offset;
  uint64_t end_time;

  retval = AcpiGetTable((ACPI_STRING)table_name, 0, (ACPI_TABLE_HEADER **)&madt_table);
  ASSERT(retval == AE_OK);
  ASSERT(madt_table->Header.Length > sizeof(acpi_table_madt));

  // Assume that the number of processors is equal to the number of LAPIC tables.
  processor_count = 0;

  // The first time through this loop, simply count the number of LAPICs, in order that we can allocate the correct
  // storage space.
  subtable = acpi_init_subtable_ptr((void *)madt_table, sizeof(acpi_table_madt));
  while(((uint64_t)subtable - (uint64_t)madt_table) < madt_table->Header.Length)
  {
    KL_TRC_TRACE(TRC_LVL::EXTRA, "Found a new table of type", (uint64_t)subtable->Type, "\n");

    if (subtable->Type == SUBTABLE_LAPIC_TYPE)
    {
      processor_count++;
    }

    subtable = acpi_advance_subtable_ptr(subtable);
  }

  KL_TRC_TRACE(TRC_LVL::EXTRA, "Number of processors", processor_count, "\n");

  proc_info_block = new processor_info[processor_count];
  inter_proc_signals = new proc_mp_ipi_msg_state[processor_count];

  // The second time around, save their details.
  subtable = acpi_init_subtable_ptr((void *)madt_table, sizeof(acpi_table_madt));
  while(((uint64_t)subtable - (uint64_t)madt_table) < madt_table->Header.Length)
  {
    KL_TRC_TRACE(TRC_LVL::EXTRA, "Found a new table of type", (uint64_t)subtable->Type, "\n");

    if (subtable->Type == SUBTABLE_LAPIC_TYPE)
    {
      // This really should never hit, unless the ACPI tables change under us!
      ASSERT(procs_saved < processor_count);

      lapic_table = (acpi_madt_local_apic *)subtable;

      proc_info_block[procs_saved].processor_id = procs_saved;
      proc_info_block[procs_saved].processor_running = false;
      proc_info_block[procs_saved].platform_data.lapic_id = lapic_table->Id;

      KL_TRC_TRACE(TRC_LVL::EXTRA, "Our processor ID", procs_saved, "\n");
      KL_TRC_TRACE(TRC_LVL::EXTRA, "ACPI proc ID", (uint64_t)lapic_table->ProcessorId, "\n");
      KL_TRC_TRACE(TRC_LVL::EXTRA, "LAPIC ID", (uint64_t)lapic_table->Id, "\n");

      procs_saved++;
    }

    subtable = acpi_advance_subtable_ptr(subtable);
  }

  // Prepare the interrupt controllers for business.
  proc_conf_interrupt_control_sys(processor_count);
  proc_conf_local_int_controller();
  proc_configure_global_int_ctrlrs();

  // This really should never hit, unless the ACPI tables change under us!
  ASSERT(procs_saved == processor_count);

  // Fill in the inter-processor signal control codes. We have to fill in a valid signal, even though it isn't actually
  // being sent, so pick an arbitrary one. Processors should be protected from acting on it through the value of
  // msg_control_state.
  for (uint32_t i = 0; i < processor_count; i++)
  {
    KL_TRC_TRACE(TRC_LVL::EXTRA, "Filling in signals for proc", i, "\n");
    inter_proc_signals[i].msg_being_sent = PROC_IPI_MSGS::SUSPEND;
    inter_proc_signals[i].msg_control_state = PROC_MP_X64_MSG_STATE::NO_MSG;
    klib_synch_spinlock_init(inter_proc_signals[i].signal_lock);
  }

  // Recreate the GDT so that it is long enough to contain TSS descriptors for all processors
  proc_recreate_gdt(processor_count);

  // Copy the real mode startup point to a suitable location = 0x1000 should be good (SIPI vector number 1).
  // Before doing this, remember that there are a couple of absolute JMP instructions that need fixing up. The first
  // is at
  trampoline_length = reinterpret_cast<uint64_t>(&asm_ap_trampoline_end) -
                      reinterpret_cast<uint64_t>(&asm_ap_trampoline_start);
  KL_TRC_TRACE(TRC_LVL::EXTRA, "Trampoline start", reinterpret_cast<uint64_t>(&asm_ap_trampoline_addr), "\n");
  KL_TRC_TRACE(TRC_LVL::EXTRA, "Trampoline length", trampoline_length, "\n");
  kl_memcpy(reinterpret_cast<void *>(&asm_ap_trampoline_addr),
            reinterpret_cast<void *>(0x1000),
            trampoline_length);

  // Signal all of the processors to wake up. They will then suspend themselves, awaiting a RESUME IPI message.
  wait_offset = time_get_system_timer_offset(10000000000); // How many HPET units is a 1-second wait?
  for (int i = 0; i < processor_count; i++)
  {
    KL_TRC_TRACE(TRC_LVL::FLOW, "Looking at processor ", i, "\n");
    if (proc_info_block[i].platform_data.lapic_id == proc_x64_apic_get_local_id() )
    {
      // This is the current processor. We know it is running.
      KL_TRC_TRACE(TRC_LVL::FLOW, "Current processor!\n");
      proc_info_block[i].processor_running = true;
    }
    else
    {
      // Boot that processor. To do this, send an INIT IPI, wait for 10ms, then send the STARTUP IPI. Make sure it
      // starts within a reasonable timeframe.
      KL_TRC_TRACE(TRC_LVL::FLOW, "Send INIT.\n");
      proc_send_ipi(proc_info_block[i].platform_data.lapic_id,
                    PROC_IPI_SHORT_TARGET::NONE,
                    PROC_IPI_INTERRUPT::INIT,
                    0,
                    true);
      KL_TRC_TRACE(TRC_LVL::FLOW, "INIT sent\n");

      // 10ms wait.
      time_stall_process(10000000);

      KL_TRC_TRACE(TRC_LVL::FLOW, "Send SIPI.\n");
      proc_send_ipi(proc_info_block[i].platform_data.lapic_id,
                    PROC_IPI_SHORT_TARGET::NONE,
                    PROC_IPI_INTERRUPT::STARTUP,
                    1, // Vector 1 indicates an entry point of 0x1000
                    true);

      // Wait for a maximum of 1-second for the processor to wake up.
      start_time = time_get_system_timer_count();
      end_time = start_time + wait_offset;

      while ((time_get_system_timer_count() < end_time) && (proc_info_block[i].processor_running == false))
      {
        // Keep waiting.
      }

      // We could probably handle this slightly more gracefully...
      ASSERT(proc_info_block[i].processor_running);
    }
  }

  // The APs have had their NMI handlers overwritten, ready to go. They are triggered in to life by proc_mp_start_aps()
  // Now all interrupt controllers needed for the BSP are good to go. Enable interrupts.
  asm_proc_start_interrupts();

  KL_TRC_EXIT;
}

/// @brief Application Processor (AP) startup code.
///
/// When this function is complete, the AP it is running on will be able to participate fully in the scheduling system.
void proc_mp_ap_startup()
{
  asm_proc_enable_fp_math();

  KL_TRC_ENTRY;

  uint32_t proc_num = proc_mp_this_proc_id();

  ASSERT(proc_num != 0);

  // Set the current task to 0, since tasking isn't started yet and we don't want to accidentally believe we're running
  // a thread that doesn't exist.
  proc_write_msr(PROC_X64_MSRS::IA32_KERNEL_GS_BASE, 0);

  // Perform generic setup tasks - the names should be self explanatory.
  asm_proc_install_idt();
  mem_x64_pat_init();
  asm_syscall_x64_prepare();
  asm_proc_load_gdt();
  proc_load_tss(proc_mp_this_proc_id());
  proc_conf_local_int_controller();

  proc_info_block[proc_num].processor_running = true;

  asm_proc_start_interrupts();

  // No need to do anything else until the task manager is kicked in to life.
  KL_TRC_TRACE(TRC_LVL::FLOW, "Waiting for scheduling\n");
  time_stall_process(2000000000);
  panic("Failed to start AP");

  KL_TRC_EXIT;
}

/// @brief Return the ID number of this processor
///
/// Until multi-processing is supported, this will always return 0.
///
/// @return The integer ID number of the processor this function executes on.
uint32_t proc_mp_this_proc_id()
{
  bool apic_id_found = false;
  uint32_t lapic_id;
  uint32_t proc_id = 0;

  KL_TRC_ENTRY;

  lapic_id = proc_x64_apic_get_local_id();

  KL_TRC_TRACE(TRC_LVL::EXTRA, "Looking for LAPIC ID", lapic_id, "\n");

  if (processor_count > 0)
  {
    KL_TRC_TRACE(TRC_LVL::FLOW, "Checking processor IDs\n");
    for (int i = 0; i < processor_count; i++)
    {
      if (lapic_id == proc_info_block[i].platform_data.lapic_id)
      {
        apic_id_found = true;
        proc_id = proc_info_block[i].processor_id;
        break;
      }
    }
  }
  else
  {
    KL_TRC_TRACE(TRC_LVL::FLOW, "Not fully init'd, assume processor 0\n");
    apic_id_found = true;
    proc_id = 0;
  }

  ASSERT(apic_id_found);
  KL_TRC_TRACE(TRC_LVL::EXTRA, "Processor ID", proc_id, "\n");

  KL_TRC_EXIT;

  return proc_id;
}

/// @brief Send a IPI signal to another processor
///
/// Inter-processor interrupts are used to signal control messages between processors. Control messages are defined in
/// PROC_IPI_MSGS. x64 processors signal each other via NMI, which doesn't carry any information with it natively. So,
/// save information in a table so that the target can look it up again.
///
/// This function waits for the target processor to acknowledge the message before continuing.
///
/// @param proc_id The processor ID (not APIC ID) to signal.
///
/// @param msg The message to be sent.
void proc_mp_x64_signal_proc(uint32_t proc_id, PROC_IPI_MSGS msg)
{
  KL_TRC_ENTRY;

  KL_TRC_TRACE(TRC_LVL::EXTRA, "Sending signal to processor", proc_id, "\n");
  KL_TRC_TRACE(TRC_LVL::EXTRA, "Message", static_cast<uint64_t>(msg), "\n");

  ASSERT(proc_id < processor_count);

  klib_synch_spinlock_lock(inter_proc_signals[proc_id].signal_lock);
  ASSERT(inter_proc_signals[proc_id].msg_control_state == PROC_MP_X64_MSG_STATE::NO_MSG);
  inter_proc_signals[proc_id].msg_being_sent = msg;
  inter_proc_signals[proc_id].msg_control_state = PROC_MP_X64_MSG_STATE::MSG_WAITING;

  proc_send_ipi(proc_info_block[proc_id].platform_data.lapic_id,
                PROC_IPI_SHORT_TARGET::NONE,
                PROC_IPI_INTERRUPT::NMI,
                0,
                false);

  while(inter_proc_signals[proc_id].msg_control_state != PROC_MP_X64_MSG_STATE::ACKNOWLEDGED)
  {
    // Spin while we wait.
  }

  inter_proc_signals[proc_id].msg_control_state = PROC_MP_X64_MSG_STATE::NO_MSG;
  klib_synch_spinlock_unlock(inter_proc_signals[proc_id].signal_lock);

  KL_TRC_EXIT;
}

/// @brief Receive and decode an IPI sent by another processor
///
/// In x64 land, inter processor signals are sent by signalling an NMI to the target. That carries no data with it, so
/// look up in the signal table to see what we received. Then pass that to the generic code to deal with it how it
/// likes
void proc_mp_x64_receive_signal_int()
{
  KL_TRC_ENTRY;

  uint32_t this_proc_id = proc_mp_this_proc_id();

  ASSERT(inter_proc_signals[this_proc_id].msg_control_state == PROC_MP_X64_MSG_STATE::MSG_WAITING);

  proc_mp_receive_signal(inter_proc_signals[this_proc_id].msg_being_sent);

  inter_proc_signals[this_proc_id].msg_control_state = PROC_MP_X64_MSG_STATE::ACKNOWLEDGED;

  KL_TRC_EXIT;
}
