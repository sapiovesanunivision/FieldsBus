// DEPRECATED forwarding header, kept for one release.
//
// The Modbus classes were renamed so the role is clear from the name (Modbus specification terms):
//   softmb::ModbusSlave        -> softmb::ModbusServer        (the PC holds the registers; the PLC polls)
//   softmb::ModbusSlaveConfig  -> softmb::ModbusServerConfig
//   ModbusSlave::masterConnected() -> ModbusServer::clientConnected()
// For the opposite role (the PC polls a PLC) see softmb::ModbusClient. README "Roles" has the full table.
#pragma once

#include "softmb/modbus_server.hpp"

namespace softmb {

using ModbusSlave [[deprecated("renamed: use softmb::ModbusServer (include softmb/modbus_server.hpp)")]] = ModbusServer;
using ModbusSlaveConfig [[deprecated("renamed: use softmb::ModbusServerConfig")]] = ModbusServerConfig;

} // namespace softmb
