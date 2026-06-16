/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/smc.h"
#include "xenia/kernel/util/shim_utils.h"

DECLARE_int32(avpack);

namespace xe {
namespace kernel {

SystemManagementController::SystemManagementController()
    : dvd_tray_state_(X_DVD_TRAY_STATE::OPEN) {
  auto registerQuery =
      [&](X_SMC_CMD command,
          void (SystemManagementController::*fn)(X_SMC_DATA*, X_SMC_DATA*)) {
        smc_commands_[command] = [this, fn](X_SMC_DATA* message,
                                            X_SMC_DATA* response) {
          (this->*fn)(message, response);
        };
      };

  registerQuery(X_SMC_CMD::QUERY_TEMP_SENSOR,
                &SystemManagementController::QueryTemperatureSensor);
  registerQuery(X_SMC_CMD::QUERY_TRAY,
                &SystemManagementController::QueryDriveTraySensor);
  registerQuery(X_SMC_CMD::QUERY_AV_PACK,
                &SystemManagementController::QueryAvPack);
  registerQuery(X_SMC_CMD::QUERY_SMC_VERSION,
                &SystemManagementController::QuerySmcVersion);
  registerQuery(X_SMC_CMD::QUERY_IR_ADDRESS,
                &SystemManagementController::QueryIRAddress);
  registerQuery(X_SMC_CMD::QUERY_TILT_SENSOR,
                &SystemManagementController::QueryTiltState);
  registerQuery(X_SMC_CMD::SET_FAN_SPEED_CPU,
                &SystemManagementController::SetFanSpeed);
  registerQuery(X_SMC_CMD::SET_FAN_SPEED_GPU,
                &SystemManagementController::SetFanSpeed);
  registerQuery(X_SMC_CMD::SET_DVD_TRAY,
                &SystemManagementController::SetDriveTray);
  registerQuery(X_SMC_CMD::SET_IR_ADDRESS,
                &SystemManagementController::SetIRAddress);
  registerQuery(X_SMC_CMD::SET_POWER_LED,
                &SystemManagementController::SetPowerLed);
  registerQuery(X_SMC_CMD::SET_LEDS, &SystemManagementController::SetLedState);
};
SystemManagementController::~SystemManagementController() {};

void SystemManagementController::SetTrayState(X_DVD_TRAY_STATE state) {
  dvd_tray_state_ = state;
  kernel_state()->BroadcastNotification(kXNotificationSystemTrayStateChanged,
                                        static_cast<uint8_t>(state));
}

void SystemManagementController::CallCommand(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  const auto itr = smc_commands_.find(smc_message->command);
  if (itr == smc_commands_.cend()) {
    XELOGW("Unimplemented SMC Command: {:02X}",
           static_cast<uint8_t>(smc_message->command));
    return;
  }

  itr->second(smc_message, smc_response);
}

void SystemManagementController::QueryTemperatureSensor(
    X_SMC_DATA* smc_message, X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->temps.cpu.SetTemp(69.6f);
  smc_response->temps.gpu.SetTemp(69.9f);
  smc_response->temps.edram.SetTemp(69.6f);
  smc_response->temps.mb.SetTemp(69.9f);
}

void SystemManagementController::QueryDriveTraySensor(
    X_SMC_DATA* smc_message, X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->dvd_tray.state = dvd_tray_state_;
};

void SystemManagementController::QueryAvPack(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->av_pack.av_pack = 0;

  const auto entry = av_pack_to_smc_value.find(cvars::avpack);
  if (entry == av_pack_to_smc_value.cend()) {
    return;
  }

  smc_response->av_pack.av_pack = entry->second;
}

void SystemManagementController::QuerySmcVersion(X_SMC_DATA* smc_message,
                                                 X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->smc_version.unk = smc_version[0];
  smc_response->smc_version.major = smc_version[1];
  smc_response->smc_version.minor = smc_version[2];
}

void SystemManagementController::QueryIRAddress(X_SMC_DATA* smc_message,
                                                X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->ir_address.ir_address = ir_address_;
}

void SystemManagementController::QueryTiltState(X_SMC_DATA* smc_message,
                                                X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->tilt_state.tilt_state = tilt_state_;
}

void SystemManagementController::SetIRAddress(X_SMC_DATA* smc_message,
                                              X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->ir_address.ir_address = ir_address_;
}

void SystemManagementController::SetDriveTray(X_SMC_DATA* smc_message,
                                              X_SMC_DATA* smc_response) {
  SetTrayState(
      static_cast<X_DVD_TRAY_STATE>((smc_message->smc_data[0] & 0xF) % 5));
}

void SystemManagementController::SetFanSpeed(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  if (smc_message->command == X_SMC_CMD::SET_FAN_SPEED_CPU) {
    cpu_fan_speed_ = (smc_message->smc_data[0] - 0x80);
  }
  if (smc_message->command == X_SMC_CMD::SET_FAN_SPEED_GPU) {
    gpu_fan_speed_ = (smc_message->smc_data[0] - 0x80);
  }
}

void SystemManagementController::SetPowerLed(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  power_led_state_ = {smc_message->power_led_state.state,
                      smc_message->power_led_state.animate};
}

void SystemManagementController::SetLedState(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  led_state_ = {smc_message->led_state.state, smc_message->led_state.region};
}

}  // namespace kernel
}  // namespace xe
