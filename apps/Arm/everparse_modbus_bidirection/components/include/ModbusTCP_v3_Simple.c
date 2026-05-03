

#include "ModbusTCP_v3_Simple.h"

uint64_t
ModbusTcpV3SimpleValidateModbusTcpFrameV3(
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
)
{
  /*  MBAP Header (7 bytes) */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusTcpFrameV3;
  if (hasBytes0)
  {
    positionAfterModbusTcpFrameV3 = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusTcpFrameV3 =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusTcpFrameV3))
  {
    res0 = positionAfterModbusTcpFrameV3;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_TCP_FRAME_V3",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusTcpFrameV3),
      EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameV3),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusTcpFrameV3;
  }
  uint64_t positionAfterTransactionId = res0;
  if (EverParseIsError(positionAfterTransactionId))
  {
    return positionAfterTransactionId;
  }
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes1 = 2ULL <= (InputLength0 - positionAfterTransactionId);
  uint64_t positionAfterProtocolId;
  if (hasBytes1)
  {
    positionAfterProtocolId = positionAfterTransactionId + 2ULL;
  }
  else
  {
    positionAfterProtocolId =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        positionAfterTransactionId);
  }
  uint64_t positionAfterModbusTcpFrameV30;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusTcpFrameV30 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_V3_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusTcpFrameV30 = positionAfterProtocolId1;
    }
    else
    {
      /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
      BOOLEAN hasBytes2 = 2ULL <= (InputLength0 - positionAfterProtocolId1);
      uint64_t positionAfterLength;
      if (hasBytes2)
      {
        positionAfterLength = positionAfterProtocolId1 + 2ULL;
      }
      else
      {
        positionAfterLength =
          EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
            positionAfterProtocolId1);
      }
      uint64_t positionAfterModbusTcpFrameV31;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusTcpFrameV31 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length >= (uint16_t)2U && length <= (uint16_t)254U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V3_SIMPLE____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusTcpFrameV31 = positionAfterLength1;
        }
        else
        {
          /*  PDU starts here */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusTcpFrameV32;
          if (hasBytes3)
          {
            positionAfterModbusTcpFrameV32 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusTcpFrameV32 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusTcpFrameV32))
          {
            res1 = positionAfterModbusTcpFrameV32;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_TCP_FRAME_V3",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusTcpFrameV32),
              EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameV32),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusTcpFrameV32;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusTcpFrameV31 = positionAfterUnitId;
          }
          else
          {
            /* Checking that we have enough space for a UINT8, i.e., 1 byte */
            BOOLEAN hasBytes4 = 1ULL <= (InputLength0 - positionAfterUnitId);
            uint64_t positionAfterFunctionCode;
            if (hasBytes4)
            {
              positionAfterFunctionCode = positionAfterUnitId + 1ULL;
            }
            else
            {
              positionAfterFunctionCode =
                EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                  positionAfterUnitId);
            }
            uint64_t positionAfterModbusTcpFrameV33;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusTcpFrameV33 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN functionCodeConstraintIsOk = functionCode >= 1U && functionCode <= 127U;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusTcpFrameV33 = positionAfterFunctionCode1;
              }
              else
              {
                /* Validating field PDUData */
                BOOLEAN
                hasBytes =
                  (uint64_t)(uint32_t)((uint32_t)length - (uint32_t)(uint16_t)2U) <=
                    (InputLength0 - positionAfterFunctionCode1);
                uint64_t res;
                if (hasBytes)
                {
                  res =
                    positionAfterFunctionCode1 +
                      (uint64_t)(uint32_t)((uint32_t)length - (uint32_t)(uint16_t)2U);
                }
                else
                {
                  res =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t positionAfterModbusTcpFrameV34 = res;
                if (EverParseIsSuccess(positionAfterModbusTcpFrameV34))
                {
                  positionAfterModbusTcpFrameV33 = positionAfterModbusTcpFrameV34;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_TCP_FRAME_V3",
                    "PDUData",
                    EverParseErrorReasonOfResult(positionAfterModbusTcpFrameV34),
                    EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameV34),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusTcpFrameV33 = positionAfterModbusTcpFrameV34;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusTcpFrameV33))
            {
              positionAfterModbusTcpFrameV31 = positionAfterModbusTcpFrameV33;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_TCP_FRAME_V3",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusTcpFrameV33),
                EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameV33),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusTcpFrameV31 = positionAfterModbusTcpFrameV33;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusTcpFrameV31))
      {
        positionAfterModbusTcpFrameV30 = positionAfterModbusTcpFrameV31;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_TCP_FRAME_V3",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusTcpFrameV31),
          EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameV31),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusTcpFrameV30 = positionAfterModbusTcpFrameV31;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusTcpFrameV30))
  {
    return positionAfterModbusTcpFrameV30;
  }
  ErrorHandlerFn("_MODBUS_TCP_FRAME_V3",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusTcpFrameV30),
    EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameV30),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusTcpFrameV30;
}

uint64_t
ModbusTcpV3SimpleValidateModbusReadRequestV3(
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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusReadRequestV3;
  if (hasBytes0)
  {
    positionAfterModbusReadRequestV3 = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusReadRequestV3 =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusReadRequestV3))
  {
    res0 = positionAfterModbusReadRequestV3;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_READ_REQUEST_V3",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusReadRequestV3),
      EverParseGetValidatorErrorKind(positionAfterModbusReadRequestV3),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusReadRequestV3;
  }
  uint64_t positionAfterTransactionId = res0;
  if (EverParseIsError(positionAfterTransactionId))
  {
    return positionAfterTransactionId;
  }
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes1 = 2ULL <= (InputLength0 - positionAfterTransactionId);
  uint64_t positionAfterProtocolId;
  if (hasBytes1)
  {
    positionAfterProtocolId = positionAfterTransactionId + 2ULL;
  }
  else
  {
    positionAfterProtocolId =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        positionAfterTransactionId);
  }
  uint64_t positionAfterModbusReadRequestV30;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusReadRequestV30 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_V3_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusReadRequestV30 = positionAfterProtocolId1;
    }
    else
    {
      /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
      BOOLEAN hasBytes2 = 2ULL <= (InputLength0 - positionAfterProtocolId1);
      uint64_t positionAfterLength;
      if (hasBytes2)
      {
        positionAfterLength = positionAfterProtocolId1 + 2ULL;
      }
      else
      {
        positionAfterLength =
          EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
            positionAfterProtocolId1);
      }
      uint64_t positionAfterModbusReadRequestV31;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusReadRequestV31 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length == (uint16_t)6U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V3_SIMPLE____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusReadRequestV31 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusReadRequestV32;
          if (hasBytes3)
          {
            positionAfterModbusReadRequestV32 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusReadRequestV32 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusReadRequestV32))
          {
            res1 = positionAfterModbusReadRequestV32;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_READ_REQUEST_V3",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusReadRequestV32),
              EverParseGetValidatorErrorKind(positionAfterModbusReadRequestV32),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusReadRequestV32;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusReadRequestV31 = positionAfterUnitId;
          }
          else
          {
            /* Checking that we have enough space for a UINT8, i.e., 1 byte */
            BOOLEAN hasBytes4 = 1ULL <= (InputLength0 - positionAfterUnitId);
            uint64_t positionAfterFunctionCode;
            if (hasBytes4)
            {
              positionAfterFunctionCode = positionAfterUnitId + 1ULL;
            }
            else
            {
              positionAfterFunctionCode =
                EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                  positionAfterUnitId);
            }
            uint64_t positionAfterModbusReadRequestV33;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusReadRequestV33 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V3_SIMPLE____FC_READ_COILS ||
                  functionCode == MODBUSTCP_V3_SIMPLE____FC_READ_HOLDING_REGISTERS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusReadRequestV33 = positionAfterFunctionCode1;
              }
              else
              {
                /* Validating field StartAddress */
                /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                BOOLEAN hasBytes5 = 2ULL <= (InputLength0 - positionAfterFunctionCode1);
                uint64_t positionAfterModbusReadRequestV34;
                if (hasBytes5)
                {
                  positionAfterModbusReadRequestV34 = positionAfterFunctionCode1 + 2ULL;
                }
                else
                {
                  positionAfterModbusReadRequestV34 =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t res;
                if (EverParseIsSuccess(positionAfterModbusReadRequestV34))
                {
                  res = positionAfterModbusReadRequestV34;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_READ_REQUEST_V3",
                    "StartAddress",
                    EverParseErrorReasonOfResult(positionAfterModbusReadRequestV34),
                    EverParseGetValidatorErrorKind(positionAfterModbusReadRequestV34),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  res = positionAfterModbusReadRequestV34;
                }
                uint64_t positionAfterStartAddress = res;
                if (EverParseIsError(positionAfterStartAddress))
                {
                  positionAfterModbusReadRequestV33 = positionAfterStartAddress;
                }
                else
                {
                  /* Validating field Quantity */
                  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                  BOOLEAN hasBytes = 2ULL <= (InputLength0 - positionAfterStartAddress);
                  uint64_t positionAfterQuantity_refinement;
                  if (hasBytes)
                  {
                    positionAfterQuantity_refinement = positionAfterStartAddress + 2ULL;
                  }
                  else
                  {
                    positionAfterQuantity_refinement =
                      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                        positionAfterStartAddress);
                  }
                  uint64_t positionAfterModbusReadRequestV35;
                  if (EverParseIsError(positionAfterQuantity_refinement))
                  {
                    positionAfterModbusReadRequestV35 = positionAfterQuantity_refinement;
                  }
                  else
                  {
                    /* reading field_value */
                    uint16_t
                    quantity_refinement = Load16Be(Input + (uint32_t)positionAfterStartAddress);
                    /* start: checking constraint */
                    BOOLEAN
                    quantity_refinementConstraintIsOk =
                      quantity_refinement >= (uint16_t)1U && quantity_refinement <= (uint16_t)125U;
                    /* end: checking constraint */
                    positionAfterModbusReadRequestV35 =
                      EverParseCheckConstraintOk(quantity_refinementConstraintIsOk,
                        positionAfterQuantity_refinement);
                  }
                  if (EverParseIsSuccess(positionAfterModbusReadRequestV35))
                  {
                    positionAfterModbusReadRequestV33 = positionAfterModbusReadRequestV35;
                  }
                  else
                  {
                    ErrorHandlerFn("_MODBUS_READ_REQUEST_V3",
                      "Quantity.refinement",
                      EverParseErrorReasonOfResult(positionAfterModbusReadRequestV35),
                      EverParseGetValidatorErrorKind(positionAfterModbusReadRequestV35),
                      Ctxt,
                      Input,
                      positionAfterStartAddress);
                    positionAfterModbusReadRequestV33 = positionAfterModbusReadRequestV35;
                  }
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusReadRequestV33))
            {
              positionAfterModbusReadRequestV31 = positionAfterModbusReadRequestV33;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_READ_REQUEST_V3",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusReadRequestV33),
                EverParseGetValidatorErrorKind(positionAfterModbusReadRequestV33),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusReadRequestV31 = positionAfterModbusReadRequestV33;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusReadRequestV31))
      {
        positionAfterModbusReadRequestV30 = positionAfterModbusReadRequestV31;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_READ_REQUEST_V3",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusReadRequestV31),
          EverParseGetValidatorErrorKind(positionAfterModbusReadRequestV31),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusReadRequestV30 = positionAfterModbusReadRequestV31;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusReadRequestV30))
  {
    return positionAfterModbusReadRequestV30;
  }
  ErrorHandlerFn("_MODBUS_READ_REQUEST_V3",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusReadRequestV30),
    EverParseGetValidatorErrorKind(positionAfterModbusReadRequestV30),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusReadRequestV30;
}

uint64_t
ModbusTcpV3SimpleValidateModbusWriteSingleRequestV3(
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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusWriteSingleRequestV3;
  if (hasBytes0)
  {
    positionAfterModbusWriteSingleRequestV3 = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusWriteSingleRequestV3 =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusWriteSingleRequestV3))
  {
    res0 = positionAfterModbusWriteSingleRequestV3;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REQUEST_V3",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRequestV3),
      EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRequestV3),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusWriteSingleRequestV3;
  }
  uint64_t positionAfterTransactionId = res0;
  if (EverParseIsError(positionAfterTransactionId))
  {
    return positionAfterTransactionId;
  }
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes1 = 2ULL <= (InputLength0 - positionAfterTransactionId);
  uint64_t positionAfterProtocolId;
  if (hasBytes1)
  {
    positionAfterProtocolId = positionAfterTransactionId + 2ULL;
  }
  else
  {
    positionAfterProtocolId =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        positionAfterTransactionId);
  }
  uint64_t positionAfterModbusWriteSingleRequestV30;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusWriteSingleRequestV30 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_V3_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusWriteSingleRequestV30 = positionAfterProtocolId1;
    }
    else
    {
      /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
      BOOLEAN hasBytes2 = 2ULL <= (InputLength0 - positionAfterProtocolId1);
      uint64_t positionAfterLength;
      if (hasBytes2)
      {
        positionAfterLength = positionAfterProtocolId1 + 2ULL;
      }
      else
      {
        positionAfterLength =
          EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
            positionAfterProtocolId1);
      }
      uint64_t positionAfterModbusWriteSingleRequestV31;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusWriteSingleRequestV31 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length == (uint16_t)6U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V3_SIMPLE____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusWriteSingleRequestV31 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusWriteSingleRequestV32;
          if (hasBytes3)
          {
            positionAfterModbusWriteSingleRequestV32 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusWriteSingleRequestV32 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusWriteSingleRequestV32))
          {
            res1 = positionAfterModbusWriteSingleRequestV32;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REQUEST_V3",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRequestV32),
              EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRequestV32),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusWriteSingleRequestV32;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusWriteSingleRequestV31 = positionAfterUnitId;
          }
          else
          {
            /* Checking that we have enough space for a UINT8, i.e., 1 byte */
            BOOLEAN hasBytes4 = 1ULL <= (InputLength0 - positionAfterUnitId);
            uint64_t positionAfterFunctionCode;
            if (hasBytes4)
            {
              positionAfterFunctionCode = positionAfterUnitId + 1ULL;
            }
            else
            {
              positionAfterFunctionCode =
                EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                  positionAfterUnitId);
            }
            uint64_t positionAfterModbusWriteSingleRequestV33;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusWriteSingleRequestV33 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V3_SIMPLE____FC_WRITE_SINGLE_REGISTER;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusWriteSingleRequestV33 = positionAfterFunctionCode1;
              }
              else
              {
                BOOLEAN hasBytes = 4ULL <= (InputLength0 - positionAfterFunctionCode1);
                uint64_t res;
                if (hasBytes)
                {
                  res = positionAfterFunctionCode1 + 4ULL;
                }
                else
                {
                  res =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                positionAfterModbusWriteSingleRequestV33 = res;
              }
            }
            if (EverParseIsSuccess(positionAfterModbusWriteSingleRequestV33))
            {
              positionAfterModbusWriteSingleRequestV31 = positionAfterModbusWriteSingleRequestV33;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REQUEST_V3",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRequestV33),
                EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRequestV33),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusWriteSingleRequestV31 = positionAfterModbusWriteSingleRequestV33;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusWriteSingleRequestV31))
      {
        positionAfterModbusWriteSingleRequestV30 = positionAfterModbusWriteSingleRequestV31;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REQUEST_V3",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRequestV31),
          EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRequestV31),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusWriteSingleRequestV30 = positionAfterModbusWriteSingleRequestV31;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusWriteSingleRequestV30))
  {
    return positionAfterModbusWriteSingleRequestV30;
  }
  ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REQUEST_V3",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRequestV30),
    EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRequestV30),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusWriteSingleRequestV30;
}

uint64_t
ModbusTcpV3SimpleValidateModbusWriteMultipleRequestV3(
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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusWriteMultipleRequestV3;
  if (hasBytes0)
  {
    positionAfterModbusWriteMultipleRequestV3 = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusWriteMultipleRequestV3 =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequestV3))
  {
    res0 = positionAfterModbusWriteMultipleRequestV3;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST_V3",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequestV3),
      EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequestV3),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusWriteMultipleRequestV3;
  }
  uint64_t positionAfterTransactionId = res0;
  if (EverParseIsError(positionAfterTransactionId))
  {
    return positionAfterTransactionId;
  }
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes1 = 2ULL <= (InputLength0 - positionAfterTransactionId);
  uint64_t positionAfterProtocolId;
  if (hasBytes1)
  {
    positionAfterProtocolId = positionAfterTransactionId + 2ULL;
  }
  else
  {
    positionAfterProtocolId =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        positionAfterTransactionId);
  }
  uint64_t positionAfterModbusWriteMultipleRequestV30;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusWriteMultipleRequestV30 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_V3_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusWriteMultipleRequestV30 = positionAfterProtocolId1;
    }
    else
    {
      /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
      BOOLEAN hasBytes2 = 2ULL <= (InputLength0 - positionAfterProtocolId1);
      uint64_t positionAfterLength;
      if (hasBytes2)
      {
        positionAfterLength = positionAfterProtocolId1 + 2ULL;
      }
      else
      {
        positionAfterLength =
          EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
            positionAfterProtocolId1);
      }
      uint64_t positionAfterModbusWriteMultipleRequestV31;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusWriteMultipleRequestV31 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length >= (uint16_t)9U && length <= (uint16_t)253U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V3_SIMPLE____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusWriteMultipleRequestV31 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusWriteMultipleRequestV32;
          if (hasBytes3)
          {
            positionAfterModbusWriteMultipleRequestV32 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusWriteMultipleRequestV32 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequestV32))
          {
            res1 = positionAfterModbusWriteMultipleRequestV32;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST_V3",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequestV32),
              EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequestV32),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusWriteMultipleRequestV32;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusWriteMultipleRequestV31 = positionAfterUnitId;
          }
          else
          {
            /* Checking that we have enough space for a UINT8, i.e., 1 byte */
            BOOLEAN hasBytes4 = 1ULL <= (InputLength0 - positionAfterUnitId);
            uint64_t positionAfterFunctionCode;
            if (hasBytes4)
            {
              positionAfterFunctionCode = positionAfterUnitId + 1ULL;
            }
            else
            {
              positionAfterFunctionCode =
                EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                  positionAfterUnitId);
            }
            uint64_t positionAfterModbusWriteMultipleRequestV33;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusWriteMultipleRequestV33 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V3_SIMPLE____FC_WRITE_MULTIPLE_REGISTERS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusWriteMultipleRequestV33 = positionAfterFunctionCode1;
              }
              else
              {
                /* Validating field StartAddress */
                /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                BOOLEAN hasBytes5 = 2ULL <= (InputLength0 - positionAfterFunctionCode1);
                uint64_t positionAfterModbusWriteMultipleRequestV34;
                if (hasBytes5)
                {
                  positionAfterModbusWriteMultipleRequestV34 = positionAfterFunctionCode1 + 2ULL;
                }
                else
                {
                  positionAfterModbusWriteMultipleRequestV34 =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t res2;
                if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequestV34))
                {
                  res2 = positionAfterModbusWriteMultipleRequestV34;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST_V3",
                    "StartAddress",
                    EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequestV34),
                    EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequestV34),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  res2 = positionAfterModbusWriteMultipleRequestV34;
                }
                uint64_t positionAfterStartAddress = res2;
                if (EverParseIsError(positionAfterStartAddress))
                {
                  positionAfterModbusWriteMultipleRequestV33 = positionAfterStartAddress;
                }
                else
                {
                  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                  BOOLEAN hasBytes6 = 2ULL <= (InputLength0 - positionAfterStartAddress);
                  uint64_t positionAfterQuantity;
                  if (hasBytes6)
                  {
                    positionAfterQuantity = positionAfterStartAddress + 2ULL;
                  }
                  else
                  {
                    positionAfterQuantity =
                      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                        positionAfterStartAddress);
                  }
                  uint64_t positionAfterModbusWriteMultipleRequestV35;
                  if (EverParseIsError(positionAfterQuantity))
                  {
                    positionAfterModbusWriteMultipleRequestV35 = positionAfterQuantity;
                  }
                  else
                  {
                    uint16_t quantity = Load16Be(Input + (uint32_t)positionAfterStartAddress);
                    BOOLEAN
                    quantityConstraintIsOk = quantity >= (uint16_t)1U && quantity <= (uint16_t)123U;
                    uint64_t
                    positionAfterQuantity1 =
                      EverParseCheckConstraintOk(quantityConstraintIsOk,
                        positionAfterQuantity);
                    if (EverParseIsError(positionAfterQuantity1))
                    {
                      positionAfterModbusWriteMultipleRequestV35 = positionAfterQuantity1;
                    }
                    else
                    {
                      /* Checking that we have enough space for a UINT8, i.e., 1 byte */
                      BOOLEAN hasBytes7 = 1ULL <= (InputLength0 - positionAfterQuantity1);
                      uint64_t positionAfterByteCount;
                      if (hasBytes7)
                      {
                        positionAfterByteCount = positionAfterQuantity1 + 1ULL;
                      }
                      else
                      {
                        positionAfterByteCount =
                          EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                            positionAfterQuantity1);
                      }
                      uint64_t positionAfterModbusWriteMultipleRequestV36;
                      if (EverParseIsError(positionAfterByteCount))
                      {
                        positionAfterModbusWriteMultipleRequestV36 = positionAfterByteCount;
                      }
                      else
                      {
                        uint8_t byteCount = Input[(uint32_t)positionAfterQuantity1];
                        BOOLEAN
                        byteCountConstraintIsOk =
                          (uint16_t)byteCount == (uint32_t)quantity * (uint32_t)(uint16_t)2U &&
                            length == (uint16_t)(7U + (uint32_t)byteCount);
                        uint64_t
                        positionAfterByteCount1 =
                          EverParseCheckConstraintOk(byteCountConstraintIsOk,
                            positionAfterByteCount);
                        if (EverParseIsError(positionAfterByteCount1))
                        {
                          positionAfterModbusWriteMultipleRequestV36 = positionAfterByteCount1;
                        }
                        else
                        {
                          /* Validating field RegisterValues */
                          BOOLEAN
                          hasBytes =
                            (uint64_t)(uint32_t)byteCount <=
                              (InputLength0 - positionAfterByteCount1);
                          uint64_t res;
                          if (hasBytes)
                          {
                            res = positionAfterByteCount1 + (uint64_t)(uint32_t)byteCount;
                          }
                          else
                          {
                            res =
                              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                                positionAfterByteCount1);
                          }
                          uint64_t positionAfterModbusWriteMultipleRequestV37 = res;
                          if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequestV37))
                          {
                            positionAfterModbusWriteMultipleRequestV36 =
                              positionAfterModbusWriteMultipleRequestV37;
                          }
                          else
                          {
                            ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST_V3",
                              "RegisterValues",
                              EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequestV37),
                              EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequestV37),
                              Ctxt,
                              Input,
                              positionAfterByteCount1);
                            positionAfterModbusWriteMultipleRequestV36 =
                              positionAfterModbusWriteMultipleRequestV37;
                          }
                        }
                      }
                      if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequestV36))
                      {
                        positionAfterModbusWriteMultipleRequestV35 =
                          positionAfterModbusWriteMultipleRequestV36;
                      }
                      else
                      {
                        ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST_V3",
                          "ByteCount",
                          EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequestV36),
                          EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequestV36),
                          Ctxt,
                          Input,
                          positionAfterQuantity1);
                        positionAfterModbusWriteMultipleRequestV35 =
                          positionAfterModbusWriteMultipleRequestV36;
                      }
                    }
                  }
                  if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequestV35))
                  {
                    positionAfterModbusWriteMultipleRequestV33 =
                      positionAfterModbusWriteMultipleRequestV35;
                  }
                  else
                  {
                    ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST_V3",
                      "Quantity",
                      EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequestV35),
                      EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequestV35),
                      Ctxt,
                      Input,
                      positionAfterStartAddress);
                    positionAfterModbusWriteMultipleRequestV33 =
                      positionAfterModbusWriteMultipleRequestV35;
                  }
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequestV33))
            {
              positionAfterModbusWriteMultipleRequestV31 =
                positionAfterModbusWriteMultipleRequestV33;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST_V3",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequestV33),
                EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequestV33),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusWriteMultipleRequestV31 =
                positionAfterModbusWriteMultipleRequestV33;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequestV31))
      {
        positionAfterModbusWriteMultipleRequestV30 = positionAfterModbusWriteMultipleRequestV31;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST_V3",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequestV31),
          EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequestV31),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusWriteMultipleRequestV30 = positionAfterModbusWriteMultipleRequestV31;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequestV30))
  {
    return positionAfterModbusWriteMultipleRequestV30;
  }
  ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST_V3",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequestV30),
    EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequestV30),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusWriteMultipleRequestV30;
}

uint64_t
ModbusTcpV3SimpleValidateModbusReadResponseV3(
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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusReadResponseV3;
  if (hasBytes0)
  {
    positionAfterModbusReadResponseV3 = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusReadResponseV3 =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusReadResponseV3))
  {
    res0 = positionAfterModbusReadResponseV3;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_READ_RESPONSE_V3",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusReadResponseV3),
      EverParseGetValidatorErrorKind(positionAfterModbusReadResponseV3),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusReadResponseV3;
  }
  uint64_t positionAfterTransactionId = res0;
  if (EverParseIsError(positionAfterTransactionId))
  {
    return positionAfterTransactionId;
  }
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes1 = 2ULL <= (InputLength0 - positionAfterTransactionId);
  uint64_t positionAfterProtocolId;
  if (hasBytes1)
  {
    positionAfterProtocolId = positionAfterTransactionId + 2ULL;
  }
  else
  {
    positionAfterProtocolId =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        positionAfterTransactionId);
  }
  uint64_t positionAfterModbusReadResponseV30;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusReadResponseV30 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_V3_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusReadResponseV30 = positionAfterProtocolId1;
    }
    else
    {
      /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
      BOOLEAN hasBytes2 = 2ULL <= (InputLength0 - positionAfterProtocolId1);
      uint64_t positionAfterLength;
      if (hasBytes2)
      {
        positionAfterLength = positionAfterProtocolId1 + 2ULL;
      }
      else
      {
        positionAfterLength =
          EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
            positionAfterProtocolId1);
      }
      uint64_t positionAfterModbusReadResponseV31;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusReadResponseV31 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length >= (uint16_t)3U && length <= (uint16_t)253U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V3_SIMPLE____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusReadResponseV31 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusReadResponseV32;
          if (hasBytes3)
          {
            positionAfterModbusReadResponseV32 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusReadResponseV32 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusReadResponseV32))
          {
            res1 = positionAfterModbusReadResponseV32;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_READ_RESPONSE_V3",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusReadResponseV32),
              EverParseGetValidatorErrorKind(positionAfterModbusReadResponseV32),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusReadResponseV32;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusReadResponseV31 = positionAfterUnitId;
          }
          else
          {
            /* Checking that we have enough space for a UINT8, i.e., 1 byte */
            BOOLEAN hasBytes4 = 1ULL <= (InputLength0 - positionAfterUnitId);
            uint64_t positionAfterFunctionCode;
            if (hasBytes4)
            {
              positionAfterFunctionCode = positionAfterUnitId + 1ULL;
            }
            else
            {
              positionAfterFunctionCode =
                EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                  positionAfterUnitId);
            }
            uint64_t positionAfterModbusReadResponseV33;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusReadResponseV33 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V3_SIMPLE____FC_READ_COILS ||
                  functionCode == MODBUSTCP_V3_SIMPLE____FC_READ_HOLDING_REGISTERS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusReadResponseV33 = positionAfterFunctionCode1;
              }
              else
              {
                /* Checking that we have enough space for a UINT8, i.e., 1 byte */
                BOOLEAN hasBytes5 = 1ULL <= (InputLength0 - positionAfterFunctionCode1);
                uint64_t positionAfterByteCount;
                if (hasBytes5)
                {
                  positionAfterByteCount = positionAfterFunctionCode1 + 1ULL;
                }
                else
                {
                  positionAfterByteCount =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t positionAfterModbusReadResponseV34;
                if (EverParseIsError(positionAfterByteCount))
                {
                  positionAfterModbusReadResponseV34 = positionAfterByteCount;
                }
                else
                {
                  uint8_t byteCount = Input[(uint32_t)positionAfterFunctionCode1];
                  BOOLEAN
                  byteCountConstraintIsOk =
                    byteCount >= 1U && byteCount <= 250U &&
                      length == (uint16_t)(3U + (uint32_t)byteCount);
                  uint64_t
                  positionAfterByteCount1 =
                    EverParseCheckConstraintOk(byteCountConstraintIsOk,
                      positionAfterByteCount);
                  if (EverParseIsError(positionAfterByteCount1))
                  {
                    positionAfterModbusReadResponseV34 = positionAfterByteCount1;
                  }
                  else
                  {
                    /* Validating field Data */
                    BOOLEAN
                    hasBytes =
                      (uint64_t)(uint32_t)byteCount <= (InputLength0 - positionAfterByteCount1);
                    uint64_t res;
                    if (hasBytes)
                    {
                      res = positionAfterByteCount1 + (uint64_t)(uint32_t)byteCount;
                    }
                    else
                    {
                      res =
                        EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                          positionAfterByteCount1);
                    }
                    uint64_t positionAfterModbusReadResponseV35 = res;
                    if (EverParseIsSuccess(positionAfterModbusReadResponseV35))
                    {
                      positionAfterModbusReadResponseV34 = positionAfterModbusReadResponseV35;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_READ_RESPONSE_V3",
                        "Data",
                        EverParseErrorReasonOfResult(positionAfterModbusReadResponseV35),
                        EverParseGetValidatorErrorKind(positionAfterModbusReadResponseV35),
                        Ctxt,
                        Input,
                        positionAfterByteCount1);
                      positionAfterModbusReadResponseV34 = positionAfterModbusReadResponseV35;
                    }
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusReadResponseV34))
                {
                  positionAfterModbusReadResponseV33 = positionAfterModbusReadResponseV34;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_READ_RESPONSE_V3",
                    "ByteCount",
                    EverParseErrorReasonOfResult(positionAfterModbusReadResponseV34),
                    EverParseGetValidatorErrorKind(positionAfterModbusReadResponseV34),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusReadResponseV33 = positionAfterModbusReadResponseV34;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusReadResponseV33))
            {
              positionAfterModbusReadResponseV31 = positionAfterModbusReadResponseV33;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_READ_RESPONSE_V3",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusReadResponseV33),
                EverParseGetValidatorErrorKind(positionAfterModbusReadResponseV33),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusReadResponseV31 = positionAfterModbusReadResponseV33;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusReadResponseV31))
      {
        positionAfterModbusReadResponseV30 = positionAfterModbusReadResponseV31;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_READ_RESPONSE_V3",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusReadResponseV31),
          EverParseGetValidatorErrorKind(positionAfterModbusReadResponseV31),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusReadResponseV30 = positionAfterModbusReadResponseV31;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusReadResponseV30))
  {
    return positionAfterModbusReadResponseV30;
  }
  ErrorHandlerFn("_MODBUS_READ_RESPONSE_V3",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusReadResponseV30),
    EverParseGetValidatorErrorKind(positionAfterModbusReadResponseV30),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusReadResponseV30;
}

uint64_t
ModbusTcpV3SimpleValidateModbusTcpFrameUnsafe(
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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength - StartPosition);
  uint64_t positionAfterModbusTcpFrameUnsafe;
  if (hasBytes0)
  {
    positionAfterModbusTcpFrameUnsafe = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusTcpFrameUnsafe =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusTcpFrameUnsafe))
  {
    res0 = positionAfterModbusTcpFrameUnsafe;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_TCP_FRAME_UNSAFE",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusTcpFrameUnsafe),
      EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameUnsafe),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusTcpFrameUnsafe;
  }
  uint64_t positionAfterTransactionId = res0;
  if (EverParseIsError(positionAfterTransactionId))
  {
    return positionAfterTransactionId;
  }
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes1 = 2ULL <= (InputLength - positionAfterTransactionId);
  uint64_t positionAfterProtocolId;
  if (hasBytes1)
  {
    positionAfterProtocolId = positionAfterTransactionId + 2ULL;
  }
  else
  {
    positionAfterProtocolId =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        positionAfterTransactionId);
  }
  uint64_t positionAfterModbusTcpFrameUnsafe0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusTcpFrameUnsafe0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_V3_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusTcpFrameUnsafe0 = positionAfterProtocolId1;
    }
    else
    {
      /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
      BOOLEAN hasBytes2 = 2ULL <= (InputLength - positionAfterProtocolId1);
      uint64_t positionAfterLength;
      if (hasBytes2)
      {
        positionAfterLength = positionAfterProtocolId1 + 2ULL;
      }
      else
      {
        positionAfterLength =
          EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
            positionAfterProtocolId1);
      }
      uint64_t positionAfterModbusTcpFrameUnsafe1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusTcpFrameUnsafe1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN lengthConstraintIsOk = length >= (uint16_t)2U && length <= (uint16_t)254U;
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusTcpFrameUnsafe1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength - positionAfterLength1);
          uint64_t positionAfterModbusTcpFrameUnsafe2;
          if (hasBytes3)
          {
            positionAfterModbusTcpFrameUnsafe2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusTcpFrameUnsafe2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusTcpFrameUnsafe2))
          {
            res1 = positionAfterModbusTcpFrameUnsafe2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_TCP_FRAME_UNSAFE",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusTcpFrameUnsafe2),
              EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameUnsafe2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusTcpFrameUnsafe2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusTcpFrameUnsafe1 = positionAfterUnitId;
          }
          else
          {
            /* Checking that we have enough space for a UINT8, i.e., 1 byte */
            BOOLEAN hasBytes4 = 1ULL <= (InputLength - positionAfterUnitId);
            uint64_t positionAfterFunctionCode;
            if (hasBytes4)
            {
              positionAfterFunctionCode = positionAfterUnitId + 1ULL;
            }
            else
            {
              positionAfterFunctionCode =
                EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                  positionAfterUnitId);
            }
            uint64_t positionAfterModbusTcpFrameUnsafe3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusTcpFrameUnsafe3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN functionCodeConstraintIsOk = functionCode >= 1U && functionCode <= 127U;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusTcpFrameUnsafe3 = positionAfterFunctionCode1;
              }
              else
              {
                /* Validating field PDUData */
                BOOLEAN
                hasBytes =
                  (uint64_t)(uint32_t)((uint32_t)length - (uint32_t)(uint16_t)2U) <=
                    (InputLength - positionAfterFunctionCode1);
                uint64_t res;
                if (hasBytes)
                {
                  res =
                    positionAfterFunctionCode1 +
                      (uint64_t)(uint32_t)((uint32_t)length - (uint32_t)(uint16_t)2U);
                }
                else
                {
                  res =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t positionAfterModbusTcpFrameUnsafe4 = res;
                if (EverParseIsSuccess(positionAfterModbusTcpFrameUnsafe4))
                {
                  positionAfterModbusTcpFrameUnsafe3 = positionAfterModbusTcpFrameUnsafe4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_TCP_FRAME_UNSAFE",
                    "PDUData",
                    EverParseErrorReasonOfResult(positionAfterModbusTcpFrameUnsafe4),
                    EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameUnsafe4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusTcpFrameUnsafe3 = positionAfterModbusTcpFrameUnsafe4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusTcpFrameUnsafe3))
            {
              positionAfterModbusTcpFrameUnsafe1 = positionAfterModbusTcpFrameUnsafe3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_TCP_FRAME_UNSAFE",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusTcpFrameUnsafe3),
                EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameUnsafe3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusTcpFrameUnsafe1 = positionAfterModbusTcpFrameUnsafe3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusTcpFrameUnsafe1))
      {
        positionAfterModbusTcpFrameUnsafe0 = positionAfterModbusTcpFrameUnsafe1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_TCP_FRAME_UNSAFE",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusTcpFrameUnsafe1),
          EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameUnsafe1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusTcpFrameUnsafe0 = positionAfterModbusTcpFrameUnsafe1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusTcpFrameUnsafe0))
  {
    return positionAfterModbusTcpFrameUnsafe0;
  }
  ErrorHandlerFn("_MODBUS_TCP_FRAME_UNSAFE",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusTcpFrameUnsafe0),
    EverParseGetValidatorErrorKind(positionAfterModbusTcpFrameUnsafe0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusTcpFrameUnsafe0;
}

