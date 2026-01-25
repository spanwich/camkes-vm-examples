

#ifndef ModbusTCP_v5_ResponsePolicy_H
#define ModbusTCP_v5_ResponsePolicy_H

#if defined(__cplusplus)
extern "C" {
#endif

#include "EverParse.h"

#define MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID (0U)

#define MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE (6U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_READ_COILS (0x01U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_READ_DISCRETE_INPUTS (0x02U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_READ_HOLDING_REGISTERS (0x03U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_READ_INPUT_REGISTERS (0x04U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_WRITE_SINGLE_COIL (0x05U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_WRITE_SINGLE_REGISTER (0x06U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_WRITE_MULTIPLE_COILS (0x0FU)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_WRITE_MULTIPLE_REGISTERS (0x10U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_READ_WRITE_MULTIPLE_REGISTERS (0x17U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_READ_COILS (0x81U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_READ_DISCRETE_INPUTS (0x82U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_READ_HOLDING_REGISTERS (0x83U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_READ_INPUT_REGISTERS (0x84U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_WRITE_SINGLE_COIL (0x85U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_WRITE_SINGLE_REGISTER (0x86U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_WRITE_MULTIPLE_COILS (0x8FU)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_WRITE_MULTIPLE_REGISTERS (0x90U)

#define MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_READ_WRITE_MULTIPLE_REGISTERS (0x97U)

#define MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_ILLEGAL_FUNCTION (0x01U)

#define MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_ILLEGAL_DATA_ADDRESS (0x02U)

#define MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_ILLEGAL_DATA_VALUE (0x03U)

#define MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_SERVER_DEVICE_FAILURE (0x04U)

#define MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_ACKNOWLEDGE (0x05U)

#define MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_SERVER_DEVICE_BUSY (0x06U)

#define MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_MEMORY_PARITY_ERROR (0x08U)

#define MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_GATEWAY_PATH_UNAVAIL (0x0AU)

#define MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_GATEWAY_TARGET_FAILED (0x0BU)

#define MODBUSTCP_V5_RESPONSEPOLICY____MAX_RESPONSE_REGISTER_BYTES (250U)

#define MODBUSTCP_V5_RESPONSEPOLICY____MAX_RESPONSE_COIL_BYTES (250U)

#define MODBUSTCP_V5_RESPONSEPOLICY____MAX_RESPONSE_DISCRETE_BYTES (250U)

#define MODBUSTCP_V5_RESPONSEPOLICY____COIL_ON (0xFF00U)

#define MODBUSTCP_V5_RESPONSEPOLICY____COIL_OFF (0x0000U)

uint64_t
ModbusTcpV5ResponsePolicyValidateModbusReadHoldingRegistersResponsePolicy(
  uint32_t InputLength,
  uint16_t MaxResponseBytes,
  uint8_t *Ctxt,
  void
  (*ErrorHandlerFn)(
    EVERPARSE_STRING x0,
    EVERPARSE_STRING x1,
    EVERPARSE_STRING x2,
    uint64_t x3,
    uint8_t *x4,
    uint8_t *x5,
    uint64_t x6
  ),
  uint8_t *Input,
  uint64_t InputLength0,
  uint64_t StartPosition
);

uint64_t
ModbusTcpV5ResponsePolicyValidateModbusReadInputRegistersResponsePolicy(
  uint32_t InputLength,
  uint16_t MaxResponseBytes,
  uint8_t *Ctxt,
  void
  (*ErrorHandlerFn)(
    EVERPARSE_STRING x0,
    EVERPARSE_STRING x1,
    EVERPARSE_STRING x2,
    uint64_t x3,
    uint8_t *x4,
    uint8_t *x5,
    uint64_t x6
  ),
  uint8_t *Input,
  uint64_t InputLength0,
  uint64_t StartPosition
);

uint64_t
ModbusTcpV5ResponsePolicyValidateModbusReadCoilsResponsePolicy(
  uint32_t InputLength,
  uint16_t MaxResponseBytes,
  uint8_t *Ctxt,
  void
  (*ErrorHandlerFn)(
    EVERPARSE_STRING x0,
    EVERPARSE_STRING x1,
    EVERPARSE_STRING x2,
    uint64_t x3,
    uint8_t *x4,
    uint8_t *x5,
    uint64_t x6
  ),
  uint8_t *Input,
  uint64_t InputLength0,
  uint64_t StartPosition
);

uint64_t
ModbusTcpV5ResponsePolicyValidateModbusReadDiscreteInputsResponsePolicy(
  uint32_t InputLength,
  uint16_t MaxResponseBytes,
  uint8_t *Ctxt,
  void
  (*ErrorHandlerFn)(
    EVERPARSE_STRING x0,
    EVERPARSE_STRING x1,
    EVERPARSE_STRING x2,
    uint64_t x3,
    uint8_t *x4,
    uint8_t *x5,
    uint64_t x6
  ),
  uint8_t *Input,
  uint64_t InputLength0,
  uint64_t StartPosition
);

uint64_t
ModbusTcpV5ResponsePolicyValidateModbusWriteSingleRegisterResponsePolicy(
  uint32_t InputLength,
  uint16_t AllowedStart,
  uint16_t AllowedEnd,
  uint8_t *Ctxt,
  void
  (*ErrorHandlerFn)(
    EVERPARSE_STRING x0,
    EVERPARSE_STRING x1,
    EVERPARSE_STRING x2,
    uint64_t x3,
    uint8_t *x4,
    uint8_t *x5,
    uint64_t x6
  ),
  uint8_t *Input,
  uint64_t InputLength0,
  uint64_t StartPosition
);

uint64_t
ModbusTcpV5ResponsePolicyValidateModbusWriteSingleCoilResponsePolicy(
  uint32_t InputLength,
  uint16_t AllowedStart,
  uint16_t AllowedEnd,
  uint8_t *Ctxt,
  void
  (*ErrorHandlerFn)(
    EVERPARSE_STRING x0,
    EVERPARSE_STRING x1,
    EVERPARSE_STRING x2,
    uint64_t x3,
    uint8_t *x4,
    uint8_t *x5,
    uint64_t x6
  ),
  uint8_t *Input,
  uint64_t InputLength0,
  uint64_t StartPosition
);

uint64_t
ModbusTcpV5ResponsePolicyValidateModbusWriteMultipleRegistersResponsePolicy(
  uint32_t InputLength,
  uint16_t AllowedStart,
  uint16_t AllowedEnd,
  uint8_t *Ctxt,
  void
  (*ErrorHandlerFn)(
    EVERPARSE_STRING x0,
    EVERPARSE_STRING x1,
    EVERPARSE_STRING x2,
    uint64_t x3,
    uint8_t *x4,
    uint8_t *x5,
    uint64_t x6
  ),
  uint8_t *Input,
  uint64_t InputLength0,
  uint64_t StartPosition
);

uint64_t
ModbusTcpV5ResponsePolicyValidateModbusWriteMultipleCoilsResponsePolicy(
  uint32_t InputLength,
  uint16_t AllowedStart,
  uint16_t AllowedEnd,
  uint8_t *Ctxt,
  void
  (*ErrorHandlerFn)(
    EVERPARSE_STRING x0,
    EVERPARSE_STRING x1,
    EVERPARSE_STRING x2,
    uint64_t x3,
    uint8_t *x4,
    uint8_t *x5,
    uint64_t x6
  ),
  uint8_t *Input,
  uint64_t InputLength0,
  uint64_t StartPosition
);

uint64_t
ModbusTcpV5ResponsePolicyValidateModbusReadWriteRegistersResponsePolicy(
  uint32_t InputLength,
  uint16_t MaxResponseBytes,
  uint8_t *Ctxt,
  void
  (*ErrorHandlerFn)(
    EVERPARSE_STRING x0,
    EVERPARSE_STRING x1,
    EVERPARSE_STRING x2,
    uint64_t x3,
    uint8_t *x4,
    uint8_t *x5,
    uint64_t x6
  ),
  uint8_t *Input,
  uint64_t InputLength0,
  uint64_t StartPosition
);

uint64_t
ModbusTcpV5ResponsePolicyValidateModbusExceptionResponsePolicy(
  uint32_t InputLength,
  uint8_t *Ctxt,
  void
  (*ErrorHandlerFn)(
    EVERPARSE_STRING x0,
    EVERPARSE_STRING x1,
    EVERPARSE_STRING x2,
    uint64_t x3,
    uint8_t *x4,
    uint8_t *x5,
    uint64_t x6
  ),
  uint8_t *Input,
  uint64_t InputLength0,
  uint64_t StartPosition
);

#if defined(__cplusplus)
}
#endif

#define ModbusTCP_v5_ResponsePolicy_H_DEFINED
#endif /* ModbusTCP_v5_ResponsePolicy_H */
