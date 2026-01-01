

#ifndef ModbusTCP_Simple_H
#define ModbusTCP_Simple_H

#if defined(__cplusplus)
extern "C" {
#endif

#include "EverParse.h"

#define MODBUSTCP_SIMPLE____MODBUS_PROTOCOL_ID (0U)

#define MODBUSTCP_SIMPLE____FC_READ_COILS (0x01U)

#define MODBUSTCP_SIMPLE____FC_READ_HOLDING_REGISTERS (0x03U)

#define MODBUSTCP_SIMPLE____FC_WRITE_SINGLE_REGISTER (0x06U)

#define MODBUSTCP_SIMPLE____FC_WRITE_MULTIPLE_REGISTERS (0x10U)

uint64_t
ModbusTcpSimpleValidateModbusTcpFrame(
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
  uint64_t InputLength,
  uint64_t StartPosition
);

uint64_t
ModbusTcpSimpleValidateModbusReadRequest(
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
  uint64_t InputLength,
  uint64_t StartPosition
);

uint64_t
ModbusTcpSimpleValidateModbusWriteSingleRequest(
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
  uint64_t InputLength,
  uint64_t StartPosition
);

uint64_t
ModbusTcpSimpleValidateModbusWriteMultipleRequest(
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
  uint64_t InputLength,
  uint64_t StartPosition
);

uint64_t
ModbusTcpSimpleValidateModbusReadResponse(
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
  uint64_t InputLength,
  uint64_t StartPosition
);

#if defined(__cplusplus)
}
#endif

#define ModbusTCP_Simple_H_DEFINED
#endif /* ModbusTCP_Simple_H */
