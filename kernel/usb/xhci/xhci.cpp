#include "usb/xhci/xhci.hpp"

int printk(const char* format, ...);

namespace {
  using namespace usb::xhci;

  Error RegisterCommandRing(Ring* ring, MemMapRegister<CRCR_Bitmap>* crcr) {
    CRCR_Bitmap value{};
    value.bits.ring_cycle_state = true;
    value.SetPointer(reinterpret_cast<uint64_t>(ring->Buffer()));
    crcr->Write(value);
    return Error::kSuccess;
  }
}

namespace usb::xhci {

  Controller::Controller(uintptr_t mmio_base)
      : mmio_base_{mmio_base},
        cap_{reinterpret_cast<CapabilityRegisters*>(mmio_base)},
        op_{reinterpret_cast<OperationalRegisters*>(
            mmio_base + cap_->CAPLENGTH.Read())},
        max_ports_{static_cast<uint8_t>(
            cap_->HCSPARAMS1.Read().bits.max_ports)} {
  }

  Error Controller::Initialize() {
    if (auto err = devmgr_.Initialize(kDeviceSize)) {
      return err;
    }

    // Host controller must be halted before resetting it.
    if (!op_->USBSTS.Read().bits.host_controller_halted) {
      auto usbcmd = op_->USBCMD.Read();
      usbcmd.bits.run_stop = false;  // stop
      op_->USBCMD.Write(usbcmd);
      while (!op_->USBSTS.Read().bits.host_controller_halted);
    }

    // Reset controller
    auto usbcmd = op_->USBCMD.Read();
    usbcmd.bits.host_controller_reset = true;
    op_->USBCMD.Write(usbcmd);
    while (op_->USBCMD.Read().bits.host_controller_reset);
    while (op_->USBSTS.Read().bits.controller_not_ready);

    //printk("MaxSlots: %u\n", cap_->HCSPARAMS1.Read().bits.max_device_slots);
    // Set "Max Slots Enabled" field in CONFIG.
    auto config = op_->CONFIG.Read();
    config.bits.max_device_slots_enabled = kDeviceSize;
    op_->CONFIG.Write(config);

    DCBAAP_Bitmap dcbaap{};
    dcbaap.SetPointer(reinterpret_cast<uint64_t>(devmgr_.DeviceContexts()));
    op_->DCBAAP.Write(dcbaap);

    auto primary_interrupter = &InterrupterRegisterSets()[0];
    if (auto err = cr_.Initialize(32)) {
        return err;
    }
    if (auto err = RegisterCommandRing(&cr_, &op_->CRCR)) {
        return err;
    }
    if (auto err = er_.Initialize(32, primary_interrupter)) {
        return err;
    }

    // Enable interrupt for the primary interrupter
    auto iman = primary_interrupter->IMAN.Read();
    iman.bits.interrupt_pending = true;
    iman.bits.interrupt_enable = true;
    primary_interrupter->IMAN.Write(iman);

    // Enable interrupt for the controller
    usbcmd = op_->USBCMD.Read();
    usbcmd.bits.interrupter_enable = true;
    op_->USBCMD.Write(usbcmd);

    return Error::kSuccess;
  }

  Error Controller::Run() {
    // Run the controller
    auto usbcmd = op_->USBCMD.Read();
    usbcmd.bits.run_stop = true;
    op_->USBCMD.Write(usbcmd);
    op_->USBCMD.Read();

    while (op_->USBSTS.Read().bits.host_controller_halted);

    return Error::kSuccess;
  }

  DoorbellRegister* Controller::DoorbellRegisterAt(uint8_t index) {
    return &DoorbellRegisters()[index];
  }

  Error AddressPort(Controller& xhc, Port& port) {
    if (!port.IsConnected()) {
      return Error::kPortNotConnected;
    }

    printk("Resetting port %d\n", port.Number());
    port.Reset();

    printk("Waiting port %d to be enabled\n", port.Number());
    while (!port.IsEnabled());

    EnableSlotCommandTRB cmd{};
    xhc.CommandRing()->Push(cmd);
    xhc.DoorbellRegisterAt(0)->Ring(0);

    printk("Waiting reply for the enable slot command\n");
    while (true) {
      while (!xhc.PrimaryEventRing()->HasFront());

      if (auto trb = TRBDynamicCast<CommandCompletionEventTRB>(
            xhc.PrimaryEventRing()->Front())) {
        if (TRBDynamicCast<EnableSlotCommandTRB>(trb->Pointer())) {
          break;
        }
        printk("event was received but it's not a response of EnableSlotCommandTRB\n");
      } else {
        printk("event was received but it's not CommandCompletionEventTRB\n");
      }

      xhc.PrimaryEventRing()->Pop();
    }

    printk("received a response of EnableSlotCommand\n");

    return Error::kSuccess;
  }
}
