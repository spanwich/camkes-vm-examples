

#include "ModbusTCP_v5_ResponsePolicy.h"

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusReadHoldingRegistersResponsePolicy;
  if (hasBytes0)
  {
    positionAfterModbusReadHoldingRegistersResponsePolicy = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusReadHoldingRegistersResponsePolicy =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusReadHoldingRegistersResponsePolicy))
  {
    res0 = positionAfterModbusReadHoldingRegistersResponsePolicy;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_READ_HOLDING_REGISTERS_RESPONSE_POLICY",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusReadHoldingRegistersResponsePolicy),
      EverParseGetValidatorErrorKind(positionAfterModbusReadHoldingRegistersResponsePolicy),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusReadHoldingRegistersResponsePolicy;
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
  uint64_t positionAfterModbusReadHoldingRegistersResponsePolicy0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusReadHoldingRegistersResponsePolicy0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk =
      protocolId == (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusReadHoldingRegistersResponsePolicy0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusReadHoldingRegistersResponsePolicy1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusReadHoldingRegistersResponsePolicy1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length >= (uint16_t)3U && length <= (uint16_t)253U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusReadHoldingRegistersResponsePolicy1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusReadHoldingRegistersResponsePolicy2;
          if (hasBytes3)
          {
            positionAfterModbusReadHoldingRegistersResponsePolicy2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusReadHoldingRegistersResponsePolicy2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusReadHoldingRegistersResponsePolicy2))
          {
            res1 = positionAfterModbusReadHoldingRegistersResponsePolicy2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_READ_HOLDING_REGISTERS_RESPONSE_POLICY",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusReadHoldingRegistersResponsePolicy2),
              EverParseGetValidatorErrorKind(positionAfterModbusReadHoldingRegistersResponsePolicy2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusReadHoldingRegistersResponsePolicy2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusReadHoldingRegistersResponsePolicy1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusReadHoldingRegistersResponsePolicy3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusReadHoldingRegistersResponsePolicy3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V5_RESPONSEPOLICY____FC_READ_HOLDING_REGISTERS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusReadHoldingRegistersResponsePolicy3 = positionAfterFunctionCode1;
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
                uint64_t positionAfterModbusReadHoldingRegistersResponsePolicy4;
                if (EverParseIsError(positionAfterByteCount))
                {
                  positionAfterModbusReadHoldingRegistersResponsePolicy4 = positionAfterByteCount;
                }
                else
                {
                  uint8_t byteCount = Input[(uint32_t)positionAfterFunctionCode1];
                  BOOLEAN
                  byteCountConstraintIsOk =
                    (uint16_t)byteCount == (uint32_t)length - (uint32_t)(uint16_t)3U &&
                      (uint32_t)byteCount % 2U == 0U
                    && byteCount <= MODBUSTCP_V5_RESPONSEPOLICY____MAX_RESPONSE_REGISTER_BYTES
                    && (uint16_t)byteCount <= MaxResponseBytes;
                  uint64_t
                  positionAfterByteCount1 =
                    EverParseCheckConstraintOk(byteCountConstraintIsOk,
                      positionAfterByteCount);
                  if (EverParseIsError(positionAfterByteCount1))
                  {
                    positionAfterModbusReadHoldingRegistersResponsePolicy4 = positionAfterByteCount1;
                  }
                  else
                  {
                    /* Validating field RegisterData */
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
                    uint64_t positionAfterModbusReadHoldingRegistersResponsePolicy5 = res;
                    if (EverParseIsSuccess(positionAfterModbusReadHoldingRegistersResponsePolicy5))
                    {
                      positionAfterModbusReadHoldingRegistersResponsePolicy4 =
                        positionAfterModbusReadHoldingRegistersResponsePolicy5;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_READ_HOLDING_REGISTERS_RESPONSE_POLICY",
                        "RegisterData",
                        EverParseErrorReasonOfResult(positionAfterModbusReadHoldingRegistersResponsePolicy5),
                        EverParseGetValidatorErrorKind(positionAfterModbusReadHoldingRegistersResponsePolicy5),
                        Ctxt,
                        Input,
                        positionAfterByteCount1);
                      positionAfterModbusReadHoldingRegistersResponsePolicy4 =
                        positionAfterModbusReadHoldingRegistersResponsePolicy5;
                    }
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusReadHoldingRegistersResponsePolicy4))
                {
                  positionAfterModbusReadHoldingRegistersResponsePolicy3 =
                    positionAfterModbusReadHoldingRegistersResponsePolicy4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_READ_HOLDING_REGISTERS_RESPONSE_POLICY",
                    "ByteCount",
                    EverParseErrorReasonOfResult(positionAfterModbusReadHoldingRegistersResponsePolicy4),
                    EverParseGetValidatorErrorKind(positionAfterModbusReadHoldingRegistersResponsePolicy4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusReadHoldingRegistersResponsePolicy3 =
                    positionAfterModbusReadHoldingRegistersResponsePolicy4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusReadHoldingRegistersResponsePolicy3))
            {
              positionAfterModbusReadHoldingRegistersResponsePolicy1 =
                positionAfterModbusReadHoldingRegistersResponsePolicy3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_READ_HOLDING_REGISTERS_RESPONSE_POLICY",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusReadHoldingRegistersResponsePolicy3),
                EverParseGetValidatorErrorKind(positionAfterModbusReadHoldingRegistersResponsePolicy3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusReadHoldingRegistersResponsePolicy1 =
                positionAfterModbusReadHoldingRegistersResponsePolicy3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusReadHoldingRegistersResponsePolicy1))
      {
        positionAfterModbusReadHoldingRegistersResponsePolicy0 =
          positionAfterModbusReadHoldingRegistersResponsePolicy1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_READ_HOLDING_REGISTERS_RESPONSE_POLICY",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusReadHoldingRegistersResponsePolicy1),
          EverParseGetValidatorErrorKind(positionAfterModbusReadHoldingRegistersResponsePolicy1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusReadHoldingRegistersResponsePolicy0 =
          positionAfterModbusReadHoldingRegistersResponsePolicy1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusReadHoldingRegistersResponsePolicy0))
  {
    return positionAfterModbusReadHoldingRegistersResponsePolicy0;
  }
  ErrorHandlerFn("_MODBUS_READ_HOLDING_REGISTERS_RESPONSE_POLICY",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusReadHoldingRegistersResponsePolicy0),
    EverParseGetValidatorErrorKind(positionAfterModbusReadHoldingRegistersResponsePolicy0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusReadHoldingRegistersResponsePolicy0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusReadInputRegistersResponsePolicy;
  if (hasBytes0)
  {
    positionAfterModbusReadInputRegistersResponsePolicy = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusReadInputRegistersResponsePolicy =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusReadInputRegistersResponsePolicy))
  {
    res0 = positionAfterModbusReadInputRegistersResponsePolicy;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_READ_INPUT_REGISTERS_RESPONSE_POLICY",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusReadInputRegistersResponsePolicy),
      EverParseGetValidatorErrorKind(positionAfterModbusReadInputRegistersResponsePolicy),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusReadInputRegistersResponsePolicy;
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
  uint64_t positionAfterModbusReadInputRegistersResponsePolicy0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusReadInputRegistersResponsePolicy0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk =
      protocolId == (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusReadInputRegistersResponsePolicy0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusReadInputRegistersResponsePolicy1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusReadInputRegistersResponsePolicy1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length >= (uint16_t)3U && length <= (uint16_t)253U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusReadInputRegistersResponsePolicy1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusReadInputRegistersResponsePolicy2;
          if (hasBytes3)
          {
            positionAfterModbusReadInputRegistersResponsePolicy2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusReadInputRegistersResponsePolicy2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusReadInputRegistersResponsePolicy2))
          {
            res1 = positionAfterModbusReadInputRegistersResponsePolicy2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_READ_INPUT_REGISTERS_RESPONSE_POLICY",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusReadInputRegistersResponsePolicy2),
              EverParseGetValidatorErrorKind(positionAfterModbusReadInputRegistersResponsePolicy2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusReadInputRegistersResponsePolicy2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusReadInputRegistersResponsePolicy1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusReadInputRegistersResponsePolicy3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusReadInputRegistersResponsePolicy3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V5_RESPONSEPOLICY____FC_READ_INPUT_REGISTERS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusReadInputRegistersResponsePolicy3 = positionAfterFunctionCode1;
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
                uint64_t positionAfterModbusReadInputRegistersResponsePolicy4;
                if (EverParseIsError(positionAfterByteCount))
                {
                  positionAfterModbusReadInputRegistersResponsePolicy4 = positionAfterByteCount;
                }
                else
                {
                  uint8_t byteCount = Input[(uint32_t)positionAfterFunctionCode1];
                  BOOLEAN
                  byteCountConstraintIsOk =
                    (uint16_t)byteCount == (uint32_t)length - (uint32_t)(uint16_t)3U &&
                      (uint32_t)byteCount % 2U == 0U
                    && byteCount <= MODBUSTCP_V5_RESPONSEPOLICY____MAX_RESPONSE_REGISTER_BYTES
                    && (uint16_t)byteCount <= MaxResponseBytes;
                  uint64_t
                  positionAfterByteCount1 =
                    EverParseCheckConstraintOk(byteCountConstraintIsOk,
                      positionAfterByteCount);
                  if (EverParseIsError(positionAfterByteCount1))
                  {
                    positionAfterModbusReadInputRegistersResponsePolicy4 = positionAfterByteCount1;
                  }
                  else
                  {
                    /* Validating field RegisterData */
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
                    uint64_t positionAfterModbusReadInputRegistersResponsePolicy5 = res;
                    if (EverParseIsSuccess(positionAfterModbusReadInputRegistersResponsePolicy5))
                    {
                      positionAfterModbusReadInputRegistersResponsePolicy4 =
                        positionAfterModbusReadInputRegistersResponsePolicy5;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_READ_INPUT_REGISTERS_RESPONSE_POLICY",
                        "RegisterData",
                        EverParseErrorReasonOfResult(positionAfterModbusReadInputRegistersResponsePolicy5),
                        EverParseGetValidatorErrorKind(positionAfterModbusReadInputRegistersResponsePolicy5),
                        Ctxt,
                        Input,
                        positionAfterByteCount1);
                      positionAfterModbusReadInputRegistersResponsePolicy4 =
                        positionAfterModbusReadInputRegistersResponsePolicy5;
                    }
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusReadInputRegistersResponsePolicy4))
                {
                  positionAfterModbusReadInputRegistersResponsePolicy3 =
                    positionAfterModbusReadInputRegistersResponsePolicy4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_READ_INPUT_REGISTERS_RESPONSE_POLICY",
                    "ByteCount",
                    EverParseErrorReasonOfResult(positionAfterModbusReadInputRegistersResponsePolicy4),
                    EverParseGetValidatorErrorKind(positionAfterModbusReadInputRegistersResponsePolicy4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusReadInputRegistersResponsePolicy3 =
                    positionAfterModbusReadInputRegistersResponsePolicy4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusReadInputRegistersResponsePolicy3))
            {
              positionAfterModbusReadInputRegistersResponsePolicy1 =
                positionAfterModbusReadInputRegistersResponsePolicy3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_READ_INPUT_REGISTERS_RESPONSE_POLICY",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusReadInputRegistersResponsePolicy3),
                EverParseGetValidatorErrorKind(positionAfterModbusReadInputRegistersResponsePolicy3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusReadInputRegistersResponsePolicy1 =
                positionAfterModbusReadInputRegistersResponsePolicy3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusReadInputRegistersResponsePolicy1))
      {
        positionAfterModbusReadInputRegistersResponsePolicy0 =
          positionAfterModbusReadInputRegistersResponsePolicy1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_READ_INPUT_REGISTERS_RESPONSE_POLICY",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusReadInputRegistersResponsePolicy1),
          EverParseGetValidatorErrorKind(positionAfterModbusReadInputRegistersResponsePolicy1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusReadInputRegistersResponsePolicy0 =
          positionAfterModbusReadInputRegistersResponsePolicy1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusReadInputRegistersResponsePolicy0))
  {
    return positionAfterModbusReadInputRegistersResponsePolicy0;
  }
  ErrorHandlerFn("_MODBUS_READ_INPUT_REGISTERS_RESPONSE_POLICY",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusReadInputRegistersResponsePolicy0),
    EverParseGetValidatorErrorKind(positionAfterModbusReadInputRegistersResponsePolicy0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusReadInputRegistersResponsePolicy0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusReadCoilsResponsePolicy;
  if (hasBytes0)
  {
    positionAfterModbusReadCoilsResponsePolicy = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusReadCoilsResponsePolicy =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusReadCoilsResponsePolicy))
  {
    res0 = positionAfterModbusReadCoilsResponsePolicy;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_READ_COILS_RESPONSE_POLICY",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusReadCoilsResponsePolicy),
      EverParseGetValidatorErrorKind(positionAfterModbusReadCoilsResponsePolicy),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusReadCoilsResponsePolicy;
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
  uint64_t positionAfterModbusReadCoilsResponsePolicy0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusReadCoilsResponsePolicy0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk =
      protocolId == (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusReadCoilsResponsePolicy0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusReadCoilsResponsePolicy1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusReadCoilsResponsePolicy1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length >= (uint16_t)3U && length <= (uint16_t)253U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusReadCoilsResponsePolicy1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusReadCoilsResponsePolicy2;
          if (hasBytes3)
          {
            positionAfterModbusReadCoilsResponsePolicy2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusReadCoilsResponsePolicy2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusReadCoilsResponsePolicy2))
          {
            res1 = positionAfterModbusReadCoilsResponsePolicy2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_READ_COILS_RESPONSE_POLICY",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusReadCoilsResponsePolicy2),
              EverParseGetValidatorErrorKind(positionAfterModbusReadCoilsResponsePolicy2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusReadCoilsResponsePolicy2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusReadCoilsResponsePolicy1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusReadCoilsResponsePolicy3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusReadCoilsResponsePolicy3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V5_RESPONSEPOLICY____FC_READ_COILS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusReadCoilsResponsePolicy3 = positionAfterFunctionCode1;
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
                uint64_t positionAfterModbusReadCoilsResponsePolicy4;
                if (EverParseIsError(positionAfterByteCount))
                {
                  positionAfterModbusReadCoilsResponsePolicy4 = positionAfterByteCount;
                }
                else
                {
                  uint8_t byteCount = Input[(uint32_t)positionAfterFunctionCode1];
                  BOOLEAN
                  byteCountConstraintIsOk =
                    (uint16_t)byteCount == (uint32_t)length - (uint32_t)(uint16_t)3U &&
                      byteCount >= 1U
                    && byteCount <= MODBUSTCP_V5_RESPONSEPOLICY____MAX_RESPONSE_COIL_BYTES
                    && (uint16_t)byteCount <= MaxResponseBytes;
                  uint64_t
                  positionAfterByteCount1 =
                    EverParseCheckConstraintOk(byteCountConstraintIsOk,
                      positionAfterByteCount);
                  if (EverParseIsError(positionAfterByteCount1))
                  {
                    positionAfterModbusReadCoilsResponsePolicy4 = positionAfterByteCount1;
                  }
                  else
                  {
                    /* Validating field CoilStatus */
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
                    uint64_t positionAfterModbusReadCoilsResponsePolicy5 = res;
                    if (EverParseIsSuccess(positionAfterModbusReadCoilsResponsePolicy5))
                    {
                      positionAfterModbusReadCoilsResponsePolicy4 =
                        positionAfterModbusReadCoilsResponsePolicy5;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_READ_COILS_RESPONSE_POLICY",
                        "CoilStatus",
                        EverParseErrorReasonOfResult(positionAfterModbusReadCoilsResponsePolicy5),
                        EverParseGetValidatorErrorKind(positionAfterModbusReadCoilsResponsePolicy5),
                        Ctxt,
                        Input,
                        positionAfterByteCount1);
                      positionAfterModbusReadCoilsResponsePolicy4 =
                        positionAfterModbusReadCoilsResponsePolicy5;
                    }
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusReadCoilsResponsePolicy4))
                {
                  positionAfterModbusReadCoilsResponsePolicy3 =
                    positionAfterModbusReadCoilsResponsePolicy4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_READ_COILS_RESPONSE_POLICY",
                    "ByteCount",
                    EverParseErrorReasonOfResult(positionAfterModbusReadCoilsResponsePolicy4),
                    EverParseGetValidatorErrorKind(positionAfterModbusReadCoilsResponsePolicy4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusReadCoilsResponsePolicy3 =
                    positionAfterModbusReadCoilsResponsePolicy4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusReadCoilsResponsePolicy3))
            {
              positionAfterModbusReadCoilsResponsePolicy1 =
                positionAfterModbusReadCoilsResponsePolicy3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_READ_COILS_RESPONSE_POLICY",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusReadCoilsResponsePolicy3),
                EverParseGetValidatorErrorKind(positionAfterModbusReadCoilsResponsePolicy3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusReadCoilsResponsePolicy1 =
                positionAfterModbusReadCoilsResponsePolicy3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusReadCoilsResponsePolicy1))
      {
        positionAfterModbusReadCoilsResponsePolicy0 = positionAfterModbusReadCoilsResponsePolicy1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_READ_COILS_RESPONSE_POLICY",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusReadCoilsResponsePolicy1),
          EverParseGetValidatorErrorKind(positionAfterModbusReadCoilsResponsePolicy1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusReadCoilsResponsePolicy0 = positionAfterModbusReadCoilsResponsePolicy1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusReadCoilsResponsePolicy0))
  {
    return positionAfterModbusReadCoilsResponsePolicy0;
  }
  ErrorHandlerFn("_MODBUS_READ_COILS_RESPONSE_POLICY",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusReadCoilsResponsePolicy0),
    EverParseGetValidatorErrorKind(positionAfterModbusReadCoilsResponsePolicy0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusReadCoilsResponsePolicy0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusReadDiscreteInputsResponsePolicy;
  if (hasBytes0)
  {
    positionAfterModbusReadDiscreteInputsResponsePolicy = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusReadDiscreteInputsResponsePolicy =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusReadDiscreteInputsResponsePolicy))
  {
    res0 = positionAfterModbusReadDiscreteInputsResponsePolicy;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_READ_DISCRETE_INPUTS_RESPONSE_POLICY",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusReadDiscreteInputsResponsePolicy),
      EverParseGetValidatorErrorKind(positionAfterModbusReadDiscreteInputsResponsePolicy),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusReadDiscreteInputsResponsePolicy;
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
  uint64_t positionAfterModbusReadDiscreteInputsResponsePolicy0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusReadDiscreteInputsResponsePolicy0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk =
      protocolId == (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusReadDiscreteInputsResponsePolicy0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusReadDiscreteInputsResponsePolicy1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusReadDiscreteInputsResponsePolicy1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length >= (uint16_t)3U && length <= (uint16_t)253U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusReadDiscreteInputsResponsePolicy1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusReadDiscreteInputsResponsePolicy2;
          if (hasBytes3)
          {
            positionAfterModbusReadDiscreteInputsResponsePolicy2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusReadDiscreteInputsResponsePolicy2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusReadDiscreteInputsResponsePolicy2))
          {
            res1 = positionAfterModbusReadDiscreteInputsResponsePolicy2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_READ_DISCRETE_INPUTS_RESPONSE_POLICY",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusReadDiscreteInputsResponsePolicy2),
              EverParseGetValidatorErrorKind(positionAfterModbusReadDiscreteInputsResponsePolicy2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusReadDiscreteInputsResponsePolicy2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusReadDiscreteInputsResponsePolicy1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusReadDiscreteInputsResponsePolicy3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusReadDiscreteInputsResponsePolicy3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V5_RESPONSEPOLICY____FC_READ_DISCRETE_INPUTS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusReadDiscreteInputsResponsePolicy3 = positionAfterFunctionCode1;
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
                uint64_t positionAfterModbusReadDiscreteInputsResponsePolicy4;
                if (EverParseIsError(positionAfterByteCount))
                {
                  positionAfterModbusReadDiscreteInputsResponsePolicy4 = positionAfterByteCount;
                }
                else
                {
                  uint8_t byteCount = Input[(uint32_t)positionAfterFunctionCode1];
                  BOOLEAN
                  byteCountConstraintIsOk =
                    (uint16_t)byteCount == (uint32_t)length - (uint32_t)(uint16_t)3U &&
                      byteCount >= 1U
                    && byteCount <= MODBUSTCP_V5_RESPONSEPOLICY____MAX_RESPONSE_DISCRETE_BYTES
                    && (uint16_t)byteCount <= MaxResponseBytes;
                  uint64_t
                  positionAfterByteCount1 =
                    EverParseCheckConstraintOk(byteCountConstraintIsOk,
                      positionAfterByteCount);
                  if (EverParseIsError(positionAfterByteCount1))
                  {
                    positionAfterModbusReadDiscreteInputsResponsePolicy4 = positionAfterByteCount1;
                  }
                  else
                  {
                    /* Validating field InputStatus */
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
                    uint64_t positionAfterModbusReadDiscreteInputsResponsePolicy5 = res;
                    if (EverParseIsSuccess(positionAfterModbusReadDiscreteInputsResponsePolicy5))
                    {
                      positionAfterModbusReadDiscreteInputsResponsePolicy4 =
                        positionAfterModbusReadDiscreteInputsResponsePolicy5;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_READ_DISCRETE_INPUTS_RESPONSE_POLICY",
                        "InputStatus",
                        EverParseErrorReasonOfResult(positionAfterModbusReadDiscreteInputsResponsePolicy5),
                        EverParseGetValidatorErrorKind(positionAfterModbusReadDiscreteInputsResponsePolicy5),
                        Ctxt,
                        Input,
                        positionAfterByteCount1);
                      positionAfterModbusReadDiscreteInputsResponsePolicy4 =
                        positionAfterModbusReadDiscreteInputsResponsePolicy5;
                    }
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusReadDiscreteInputsResponsePolicy4))
                {
                  positionAfterModbusReadDiscreteInputsResponsePolicy3 =
                    positionAfterModbusReadDiscreteInputsResponsePolicy4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_READ_DISCRETE_INPUTS_RESPONSE_POLICY",
                    "ByteCount",
                    EverParseErrorReasonOfResult(positionAfterModbusReadDiscreteInputsResponsePolicy4),
                    EverParseGetValidatorErrorKind(positionAfterModbusReadDiscreteInputsResponsePolicy4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusReadDiscreteInputsResponsePolicy3 =
                    positionAfterModbusReadDiscreteInputsResponsePolicy4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusReadDiscreteInputsResponsePolicy3))
            {
              positionAfterModbusReadDiscreteInputsResponsePolicy1 =
                positionAfterModbusReadDiscreteInputsResponsePolicy3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_READ_DISCRETE_INPUTS_RESPONSE_POLICY",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusReadDiscreteInputsResponsePolicy3),
                EverParseGetValidatorErrorKind(positionAfterModbusReadDiscreteInputsResponsePolicy3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusReadDiscreteInputsResponsePolicy1 =
                positionAfterModbusReadDiscreteInputsResponsePolicy3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusReadDiscreteInputsResponsePolicy1))
      {
        positionAfterModbusReadDiscreteInputsResponsePolicy0 =
          positionAfterModbusReadDiscreteInputsResponsePolicy1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_READ_DISCRETE_INPUTS_RESPONSE_POLICY",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusReadDiscreteInputsResponsePolicy1),
          EverParseGetValidatorErrorKind(positionAfterModbusReadDiscreteInputsResponsePolicy1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusReadDiscreteInputsResponsePolicy0 =
          positionAfterModbusReadDiscreteInputsResponsePolicy1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusReadDiscreteInputsResponsePolicy0))
  {
    return positionAfterModbusReadDiscreteInputsResponsePolicy0;
  }
  ErrorHandlerFn("_MODBUS_READ_DISCRETE_INPUTS_RESPONSE_POLICY",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusReadDiscreteInputsResponsePolicy0),
    EverParseGetValidatorErrorKind(positionAfterModbusReadDiscreteInputsResponsePolicy0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusReadDiscreteInputsResponsePolicy0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusWriteSingleRegisterResponsePolicy;
  if (hasBytes0)
  {
    positionAfterModbusWriteSingleRegisterResponsePolicy = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusWriteSingleRegisterResponsePolicy =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusWriteSingleRegisterResponsePolicy))
  {
    res0 = positionAfterModbusWriteSingleRegisterResponsePolicy;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REGISTER_RESPONSE_POLICY",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRegisterResponsePolicy),
      EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRegisterResponsePolicy),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusWriteSingleRegisterResponsePolicy;
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
  uint64_t positionAfterModbusWriteSingleRegisterResponsePolicy0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusWriteSingleRegisterResponsePolicy0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk =
      protocolId == (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusWriteSingleRegisterResponsePolicy0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusWriteSingleRegisterResponsePolicy1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusWriteSingleRegisterResponsePolicy1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length == (uint16_t)6U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusWriteSingleRegisterResponsePolicy1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusWriteSingleRegisterResponsePolicy2;
          if (hasBytes3)
          {
            positionAfterModbusWriteSingleRegisterResponsePolicy2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusWriteSingleRegisterResponsePolicy2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusWriteSingleRegisterResponsePolicy2))
          {
            res1 = positionAfterModbusWriteSingleRegisterResponsePolicy2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REGISTER_RESPONSE_POLICY",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRegisterResponsePolicy2),
              EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRegisterResponsePolicy2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusWriteSingleRegisterResponsePolicy2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusWriteSingleRegisterResponsePolicy1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusWriteSingleRegisterResponsePolicy3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusWriteSingleRegisterResponsePolicy3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V5_RESPONSEPOLICY____FC_WRITE_SINGLE_REGISTER;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusWriteSingleRegisterResponsePolicy3 = positionAfterFunctionCode1;
              }
              else
              {
                /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                BOOLEAN hasBytes5 = 2ULL <= (InputLength0 - positionAfterFunctionCode1);
                uint64_t positionAfterRegisterAddress;
                if (hasBytes5)
                {
                  positionAfterRegisterAddress = positionAfterFunctionCode1 + 2ULL;
                }
                else
                {
                  positionAfterRegisterAddress =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t positionAfterModbusWriteSingleRegisterResponsePolicy4;
                if (EverParseIsError(positionAfterRegisterAddress))
                {
                  positionAfterModbusWriteSingleRegisterResponsePolicy4 =
                    positionAfterRegisterAddress;
                }
                else
                {
                  uint16_t registerAddress = Load16Be(Input + (uint32_t)positionAfterFunctionCode1);
                  BOOLEAN
                  registerAddressConstraintIsOk =
                    registerAddress >= AllowedStart && registerAddress <= AllowedEnd;
                  uint64_t
                  positionAfterRegisterAddress1 =
                    EverParseCheckConstraintOk(registerAddressConstraintIsOk,
                      positionAfterRegisterAddress);
                  if (EverParseIsError(positionAfterRegisterAddress1))
                  {
                    positionAfterModbusWriteSingleRegisterResponsePolicy4 =
                      positionAfterRegisterAddress1;
                  }
                  else
                  {
                    /* Validating field RegisterValue */
                    /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                    BOOLEAN hasBytes = 2ULL <= (InputLength0 - positionAfterRegisterAddress1);
                    uint64_t positionAfterModbusWriteSingleRegisterResponsePolicy5;
                    if (hasBytes)
                    {
                      positionAfterModbusWriteSingleRegisterResponsePolicy5 =
                        positionAfterRegisterAddress1 + 2ULL;
                    }
                    else
                    {
                      positionAfterModbusWriteSingleRegisterResponsePolicy5 =
                        EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                          positionAfterRegisterAddress1);
                    }
                    uint64_t res;
                    if (EverParseIsSuccess(positionAfterModbusWriteSingleRegisterResponsePolicy5))
                    {
                      res = positionAfterModbusWriteSingleRegisterResponsePolicy5;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REGISTER_RESPONSE_POLICY",
                        "RegisterValue",
                        EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRegisterResponsePolicy5),
                        EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRegisterResponsePolicy5),
                        Ctxt,
                        Input,
                        positionAfterRegisterAddress1);
                      res = positionAfterModbusWriteSingleRegisterResponsePolicy5;
                    }
                    positionAfterModbusWriteSingleRegisterResponsePolicy4 = res;
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusWriteSingleRegisterResponsePolicy4))
                {
                  positionAfterModbusWriteSingleRegisterResponsePolicy3 =
                    positionAfterModbusWriteSingleRegisterResponsePolicy4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REGISTER_RESPONSE_POLICY",
                    "RegisterAddress",
                    EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRegisterResponsePolicy4),
                    EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRegisterResponsePolicy4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusWriteSingleRegisterResponsePolicy3 =
                    positionAfterModbusWriteSingleRegisterResponsePolicy4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusWriteSingleRegisterResponsePolicy3))
            {
              positionAfterModbusWriteSingleRegisterResponsePolicy1 =
                positionAfterModbusWriteSingleRegisterResponsePolicy3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REGISTER_RESPONSE_POLICY",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRegisterResponsePolicy3),
                EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRegisterResponsePolicy3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusWriteSingleRegisterResponsePolicy1 =
                positionAfterModbusWriteSingleRegisterResponsePolicy3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusWriteSingleRegisterResponsePolicy1))
      {
        positionAfterModbusWriteSingleRegisterResponsePolicy0 =
          positionAfterModbusWriteSingleRegisterResponsePolicy1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REGISTER_RESPONSE_POLICY",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRegisterResponsePolicy1),
          EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRegisterResponsePolicy1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusWriteSingleRegisterResponsePolicy0 =
          positionAfterModbusWriteSingleRegisterResponsePolicy1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusWriteSingleRegisterResponsePolicy0))
  {
    return positionAfterModbusWriteSingleRegisterResponsePolicy0;
  }
  ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REGISTER_RESPONSE_POLICY",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRegisterResponsePolicy0),
    EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRegisterResponsePolicy0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusWriteSingleRegisterResponsePolicy0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusWriteSingleCoilResponsePolicy;
  if (hasBytes0)
  {
    positionAfterModbusWriteSingleCoilResponsePolicy = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusWriteSingleCoilResponsePolicy =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusWriteSingleCoilResponsePolicy))
  {
    res0 = positionAfterModbusWriteSingleCoilResponsePolicy;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_WRITE_SINGLE_COIL_RESPONSE_POLICY",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusWriteSingleCoilResponsePolicy),
      EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleCoilResponsePolicy),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusWriteSingleCoilResponsePolicy;
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
  uint64_t positionAfterModbusWriteSingleCoilResponsePolicy0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusWriteSingleCoilResponsePolicy0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk =
      protocolId == (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusWriteSingleCoilResponsePolicy0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusWriteSingleCoilResponsePolicy1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusWriteSingleCoilResponsePolicy1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length == (uint16_t)6U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusWriteSingleCoilResponsePolicy1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusWriteSingleCoilResponsePolicy2;
          if (hasBytes3)
          {
            positionAfterModbusWriteSingleCoilResponsePolicy2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusWriteSingleCoilResponsePolicy2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res;
          if (EverParseIsSuccess(positionAfterModbusWriteSingleCoilResponsePolicy2))
          {
            res = positionAfterModbusWriteSingleCoilResponsePolicy2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_WRITE_SINGLE_COIL_RESPONSE_POLICY",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusWriteSingleCoilResponsePolicy2),
              EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleCoilResponsePolicy2),
              Ctxt,
              Input,
              positionAfterLength1);
            res = positionAfterModbusWriteSingleCoilResponsePolicy2;
          }
          uint64_t positionAfterUnitId = res;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusWriteSingleCoilResponsePolicy1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusWriteSingleCoilResponsePolicy3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusWriteSingleCoilResponsePolicy3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V5_RESPONSEPOLICY____FC_WRITE_SINGLE_COIL;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusWriteSingleCoilResponsePolicy3 = positionAfterFunctionCode1;
              }
              else
              {
                /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                BOOLEAN hasBytes5 = 2ULL <= (InputLength0 - positionAfterFunctionCode1);
                uint64_t positionAfterOutputAddress;
                if (hasBytes5)
                {
                  positionAfterOutputAddress = positionAfterFunctionCode1 + 2ULL;
                }
                else
                {
                  positionAfterOutputAddress =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t positionAfterModbusWriteSingleCoilResponsePolicy4;
                if (EverParseIsError(positionAfterOutputAddress))
                {
                  positionAfterModbusWriteSingleCoilResponsePolicy4 = positionAfterOutputAddress;
                }
                else
                {
                  uint16_t outputAddress = Load16Be(Input + (uint32_t)positionAfterFunctionCode1);
                  BOOLEAN
                  outputAddressConstraintIsOk =
                    outputAddress >= AllowedStart && outputAddress <= AllowedEnd;
                  uint64_t
                  positionAfterOutputAddress1 =
                    EverParseCheckConstraintOk(outputAddressConstraintIsOk,
                      positionAfterOutputAddress);
                  if (EverParseIsError(positionAfterOutputAddress1))
                  {
                    positionAfterModbusWriteSingleCoilResponsePolicy4 = positionAfterOutputAddress1;
                  }
                  else
                  {
                    /* Validating field OutputValue */
                    /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                    BOOLEAN hasBytes = 2ULL <= (InputLength0 - positionAfterOutputAddress1);
                    uint64_t positionAfterOutputValue_refinement;
                    if (hasBytes)
                    {
                      positionAfterOutputValue_refinement = positionAfterOutputAddress1 + 2ULL;
                    }
                    else
                    {
                      positionAfterOutputValue_refinement =
                        EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                          positionAfterOutputAddress1);
                    }
                    uint64_t positionAfterModbusWriteSingleCoilResponsePolicy5;
                    if (EverParseIsError(positionAfterOutputValue_refinement))
                    {
                      positionAfterModbusWriteSingleCoilResponsePolicy5 =
                        positionAfterOutputValue_refinement;
                    }
                    else
                    {
                      /* reading field_value */
                      uint16_t
                      outputValue_refinement =
                        Load16Be(Input + (uint32_t)positionAfterOutputAddress1);
                      /* start: checking constraint */
                      BOOLEAN
                      outputValue_refinementConstraintIsOk =
                        outputValue_refinement == MODBUSTCP_V5_RESPONSEPOLICY____COIL_ON ||
                          outputValue_refinement ==
                            (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____COIL_OFF;
                      /* end: checking constraint */
                      positionAfterModbusWriteSingleCoilResponsePolicy5 =
                        EverParseCheckConstraintOk(outputValue_refinementConstraintIsOk,
                          positionAfterOutputValue_refinement);
                    }
                    if (EverParseIsSuccess(positionAfterModbusWriteSingleCoilResponsePolicy5))
                    {
                      positionAfterModbusWriteSingleCoilResponsePolicy4 =
                        positionAfterModbusWriteSingleCoilResponsePolicy5;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_WRITE_SINGLE_COIL_RESPONSE_POLICY",
                        "OutputValue.refinement",
                        EverParseErrorReasonOfResult(positionAfterModbusWriteSingleCoilResponsePolicy5),
                        EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleCoilResponsePolicy5),
                        Ctxt,
                        Input,
                        positionAfterOutputAddress1);
                      positionAfterModbusWriteSingleCoilResponsePolicy4 =
                        positionAfterModbusWriteSingleCoilResponsePolicy5;
                    }
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusWriteSingleCoilResponsePolicy4))
                {
                  positionAfterModbusWriteSingleCoilResponsePolicy3 =
                    positionAfterModbusWriteSingleCoilResponsePolicy4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_WRITE_SINGLE_COIL_RESPONSE_POLICY",
                    "OutputAddress",
                    EverParseErrorReasonOfResult(positionAfterModbusWriteSingleCoilResponsePolicy4),
                    EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleCoilResponsePolicy4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusWriteSingleCoilResponsePolicy3 =
                    positionAfterModbusWriteSingleCoilResponsePolicy4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusWriteSingleCoilResponsePolicy3))
            {
              positionAfterModbusWriteSingleCoilResponsePolicy1 =
                positionAfterModbusWriteSingleCoilResponsePolicy3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_WRITE_SINGLE_COIL_RESPONSE_POLICY",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusWriteSingleCoilResponsePolicy3),
                EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleCoilResponsePolicy3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusWriteSingleCoilResponsePolicy1 =
                positionAfterModbusWriteSingleCoilResponsePolicy3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusWriteSingleCoilResponsePolicy1))
      {
        positionAfterModbusWriteSingleCoilResponsePolicy0 =
          positionAfterModbusWriteSingleCoilResponsePolicy1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_WRITE_SINGLE_COIL_RESPONSE_POLICY",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusWriteSingleCoilResponsePolicy1),
          EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleCoilResponsePolicy1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusWriteSingleCoilResponsePolicy0 =
          positionAfterModbusWriteSingleCoilResponsePolicy1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusWriteSingleCoilResponsePolicy0))
  {
    return positionAfterModbusWriteSingleCoilResponsePolicy0;
  }
  ErrorHandlerFn("_MODBUS_WRITE_SINGLE_COIL_RESPONSE_POLICY",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusWriteSingleCoilResponsePolicy0),
    EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleCoilResponsePolicy0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusWriteSingleCoilResponsePolicy0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusWriteMultipleRegistersResponsePolicy;
  if (hasBytes0)
  {
    positionAfterModbusWriteMultipleRegistersResponsePolicy = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusWriteMultipleRegistersResponsePolicy =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusWriteMultipleRegistersResponsePolicy))
  {
    res0 = positionAfterModbusWriteMultipleRegistersResponsePolicy;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REGISTERS_RESPONSE_POLICY",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRegistersResponsePolicy),
      EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRegistersResponsePolicy),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusWriteMultipleRegistersResponsePolicy;
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
  uint64_t positionAfterModbusWriteMultipleRegistersResponsePolicy0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusWriteMultipleRegistersResponsePolicy0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk =
      protocolId == (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusWriteMultipleRegistersResponsePolicy0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusWriteMultipleRegistersResponsePolicy1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusWriteMultipleRegistersResponsePolicy1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length == (uint16_t)6U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusWriteMultipleRegistersResponsePolicy1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusWriteMultipleRegistersResponsePolicy2;
          if (hasBytes3)
          {
            positionAfterModbusWriteMultipleRegistersResponsePolicy2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusWriteMultipleRegistersResponsePolicy2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res;
          if (EverParseIsSuccess(positionAfterModbusWriteMultipleRegistersResponsePolicy2))
          {
            res = positionAfterModbusWriteMultipleRegistersResponsePolicy2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REGISTERS_RESPONSE_POLICY",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRegistersResponsePolicy2),
              EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRegistersResponsePolicy2),
              Ctxt,
              Input,
              positionAfterLength1);
            res = positionAfterModbusWriteMultipleRegistersResponsePolicy2;
          }
          uint64_t positionAfterUnitId = res;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusWriteMultipleRegistersResponsePolicy1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusWriteMultipleRegistersResponsePolicy3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusWriteMultipleRegistersResponsePolicy3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V5_RESPONSEPOLICY____FC_WRITE_MULTIPLE_REGISTERS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusWriteMultipleRegistersResponsePolicy3 =
                  positionAfterFunctionCode1;
              }
              else
              {
                /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                BOOLEAN hasBytes5 = 2ULL <= (InputLength0 - positionAfterFunctionCode1);
                uint64_t positionAfterStartAddress;
                if (hasBytes5)
                {
                  positionAfterStartAddress = positionAfterFunctionCode1 + 2ULL;
                }
                else
                {
                  positionAfterStartAddress =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t positionAfterModbusWriteMultipleRegistersResponsePolicy4;
                if (EverParseIsError(positionAfterStartAddress))
                {
                  positionAfterModbusWriteMultipleRegistersResponsePolicy4 =
                    positionAfterStartAddress;
                }
                else
                {
                  uint16_t startAddress = Load16Be(Input + (uint32_t)positionAfterFunctionCode1);
                  BOOLEAN
                  startAddressConstraintIsOk =
                    startAddress >= AllowedStart && startAddress <= AllowedEnd;
                  uint64_t
                  positionAfterStartAddress1 =
                    EverParseCheckConstraintOk(startAddressConstraintIsOk,
                      positionAfterStartAddress);
                  if (EverParseIsError(positionAfterStartAddress1))
                  {
                    positionAfterModbusWriteMultipleRegistersResponsePolicy4 =
                      positionAfterStartAddress1;
                  }
                  else
                  {
                    /* Validating field Quantity */
                    /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                    BOOLEAN hasBytes = 2ULL <= (InputLength0 - positionAfterStartAddress1);
                    uint64_t positionAfterQuantity_refinement;
                    if (hasBytes)
                    {
                      positionAfterQuantity_refinement = positionAfterStartAddress1 + 2ULL;
                    }
                    else
                    {
                      positionAfterQuantity_refinement =
                        EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                          positionAfterStartAddress1);
                    }
                    uint64_t positionAfterModbusWriteMultipleRegistersResponsePolicy5;
                    if (EverParseIsError(positionAfterQuantity_refinement))
                    {
                      positionAfterModbusWriteMultipleRegistersResponsePolicy5 =
                        positionAfterQuantity_refinement;
                    }
                    else
                    {
                      /* reading field_value */
                      uint16_t
                      quantity_refinement = Load16Be(Input + (uint32_t)positionAfterStartAddress1);
                      /* start: checking constraint */
                      BOOLEAN
                      quantity_refinementConstraintIsOk =
                        quantity_refinement >= (uint16_t)1U && quantity_refinement <= (uint16_t)123U
                        &&
                          ((uint32_t)quantity_refinement - (uint32_t)(uint16_t)1U) <=
                            ((uint32_t)AllowedEnd - (uint32_t)startAddress);
                      /* end: checking constraint */
                      positionAfterModbusWriteMultipleRegistersResponsePolicy5 =
                        EverParseCheckConstraintOk(quantity_refinementConstraintIsOk,
                          positionAfterQuantity_refinement);
                    }
                    if
                    (EverParseIsSuccess(positionAfterModbusWriteMultipleRegistersResponsePolicy5))
                    {
                      positionAfterModbusWriteMultipleRegistersResponsePolicy4 =
                        positionAfterModbusWriteMultipleRegistersResponsePolicy5;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REGISTERS_RESPONSE_POLICY",
                        "Quantity.refinement",
                        EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRegistersResponsePolicy5),
                        EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRegistersResponsePolicy5),
                        Ctxt,
                        Input,
                        positionAfterStartAddress1);
                      positionAfterModbusWriteMultipleRegistersResponsePolicy4 =
                        positionAfterModbusWriteMultipleRegistersResponsePolicy5;
                    }
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusWriteMultipleRegistersResponsePolicy4))
                {
                  positionAfterModbusWriteMultipleRegistersResponsePolicy3 =
                    positionAfterModbusWriteMultipleRegistersResponsePolicy4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REGISTERS_RESPONSE_POLICY",
                    "StartAddress",
                    EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRegistersResponsePolicy4),
                    EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRegistersResponsePolicy4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusWriteMultipleRegistersResponsePolicy3 =
                    positionAfterModbusWriteMultipleRegistersResponsePolicy4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusWriteMultipleRegistersResponsePolicy3))
            {
              positionAfterModbusWriteMultipleRegistersResponsePolicy1 =
                positionAfterModbusWriteMultipleRegistersResponsePolicy3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REGISTERS_RESPONSE_POLICY",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRegistersResponsePolicy3),
                EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRegistersResponsePolicy3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusWriteMultipleRegistersResponsePolicy1 =
                positionAfterModbusWriteMultipleRegistersResponsePolicy3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusWriteMultipleRegistersResponsePolicy1))
      {
        positionAfterModbusWriteMultipleRegistersResponsePolicy0 =
          positionAfterModbusWriteMultipleRegistersResponsePolicy1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REGISTERS_RESPONSE_POLICY",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRegistersResponsePolicy1),
          EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRegistersResponsePolicy1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusWriteMultipleRegistersResponsePolicy0 =
          positionAfterModbusWriteMultipleRegistersResponsePolicy1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusWriteMultipleRegistersResponsePolicy0))
  {
    return positionAfterModbusWriteMultipleRegistersResponsePolicy0;
  }
  ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REGISTERS_RESPONSE_POLICY",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRegistersResponsePolicy0),
    EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRegistersResponsePolicy0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusWriteMultipleRegistersResponsePolicy0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusWriteMultipleCoilsResponsePolicy;
  if (hasBytes0)
  {
    positionAfterModbusWriteMultipleCoilsResponsePolicy = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusWriteMultipleCoilsResponsePolicy =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusWriteMultipleCoilsResponsePolicy))
  {
    res0 = positionAfterModbusWriteMultipleCoilsResponsePolicy;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_COILS_RESPONSE_POLICY",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleCoilsResponsePolicy),
      EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleCoilsResponsePolicy),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusWriteMultipleCoilsResponsePolicy;
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
  uint64_t positionAfterModbusWriteMultipleCoilsResponsePolicy0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusWriteMultipleCoilsResponsePolicy0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk =
      protocolId == (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusWriteMultipleCoilsResponsePolicy0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusWriteMultipleCoilsResponsePolicy1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusWriteMultipleCoilsResponsePolicy1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length == (uint16_t)6U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusWriteMultipleCoilsResponsePolicy1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusWriteMultipleCoilsResponsePolicy2;
          if (hasBytes3)
          {
            positionAfterModbusWriteMultipleCoilsResponsePolicy2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusWriteMultipleCoilsResponsePolicy2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res;
          if (EverParseIsSuccess(positionAfterModbusWriteMultipleCoilsResponsePolicy2))
          {
            res = positionAfterModbusWriteMultipleCoilsResponsePolicy2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_COILS_RESPONSE_POLICY",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleCoilsResponsePolicy2),
              EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleCoilsResponsePolicy2),
              Ctxt,
              Input,
              positionAfterLength1);
            res = positionAfterModbusWriteMultipleCoilsResponsePolicy2;
          }
          uint64_t positionAfterUnitId = res;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusWriteMultipleCoilsResponsePolicy1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusWriteMultipleCoilsResponsePolicy3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusWriteMultipleCoilsResponsePolicy3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V5_RESPONSEPOLICY____FC_WRITE_MULTIPLE_COILS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusWriteMultipleCoilsResponsePolicy3 = positionAfterFunctionCode1;
              }
              else
              {
                /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                BOOLEAN hasBytes5 = 2ULL <= (InputLength0 - positionAfterFunctionCode1);
                uint64_t positionAfterStartAddress;
                if (hasBytes5)
                {
                  positionAfterStartAddress = positionAfterFunctionCode1 + 2ULL;
                }
                else
                {
                  positionAfterStartAddress =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t positionAfterModbusWriteMultipleCoilsResponsePolicy4;
                if (EverParseIsError(positionAfterStartAddress))
                {
                  positionAfterModbusWriteMultipleCoilsResponsePolicy4 = positionAfterStartAddress;
                }
                else
                {
                  uint16_t startAddress = Load16Be(Input + (uint32_t)positionAfterFunctionCode1);
                  BOOLEAN
                  startAddressConstraintIsOk =
                    startAddress >= AllowedStart && startAddress <= AllowedEnd;
                  uint64_t
                  positionAfterStartAddress1 =
                    EverParseCheckConstraintOk(startAddressConstraintIsOk,
                      positionAfterStartAddress);
                  if (EverParseIsError(positionAfterStartAddress1))
                  {
                    positionAfterModbusWriteMultipleCoilsResponsePolicy4 =
                      positionAfterStartAddress1;
                  }
                  else
                  {
                    /* Validating field Quantity */
                    /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                    BOOLEAN hasBytes = 2ULL <= (InputLength0 - positionAfterStartAddress1);
                    uint64_t positionAfterQuantity_refinement;
                    if (hasBytes)
                    {
                      positionAfterQuantity_refinement = positionAfterStartAddress1 + 2ULL;
                    }
                    else
                    {
                      positionAfterQuantity_refinement =
                        EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                          positionAfterStartAddress1);
                    }
                    uint64_t positionAfterModbusWriteMultipleCoilsResponsePolicy5;
                    if (EverParseIsError(positionAfterQuantity_refinement))
                    {
                      positionAfterModbusWriteMultipleCoilsResponsePolicy5 =
                        positionAfterQuantity_refinement;
                    }
                    else
                    {
                      /* reading field_value */
                      uint16_t
                      quantity_refinement = Load16Be(Input + (uint32_t)positionAfterStartAddress1);
                      /* start: checking constraint */
                      BOOLEAN
                      quantity_refinementConstraintIsOk =
                        quantity_refinement >= (uint16_t)1U && quantity_refinement <= 1968U &&
                          ((uint32_t)quantity_refinement - (uint32_t)(uint16_t)1U) <=
                            ((uint32_t)AllowedEnd - (uint32_t)startAddress);
                      /* end: checking constraint */
                      positionAfterModbusWriteMultipleCoilsResponsePolicy5 =
                        EverParseCheckConstraintOk(quantity_refinementConstraintIsOk,
                          positionAfterQuantity_refinement);
                    }
                    if (EverParseIsSuccess(positionAfterModbusWriteMultipleCoilsResponsePolicy5))
                    {
                      positionAfterModbusWriteMultipleCoilsResponsePolicy4 =
                        positionAfterModbusWriteMultipleCoilsResponsePolicy5;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_COILS_RESPONSE_POLICY",
                        "Quantity.refinement",
                        EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleCoilsResponsePolicy5),
                        EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleCoilsResponsePolicy5),
                        Ctxt,
                        Input,
                        positionAfterStartAddress1);
                      positionAfterModbusWriteMultipleCoilsResponsePolicy4 =
                        positionAfterModbusWriteMultipleCoilsResponsePolicy5;
                    }
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusWriteMultipleCoilsResponsePolicy4))
                {
                  positionAfterModbusWriteMultipleCoilsResponsePolicy3 =
                    positionAfterModbusWriteMultipleCoilsResponsePolicy4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_COILS_RESPONSE_POLICY",
                    "StartAddress",
                    EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleCoilsResponsePolicy4),
                    EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleCoilsResponsePolicy4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusWriteMultipleCoilsResponsePolicy3 =
                    positionAfterModbusWriteMultipleCoilsResponsePolicy4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusWriteMultipleCoilsResponsePolicy3))
            {
              positionAfterModbusWriteMultipleCoilsResponsePolicy1 =
                positionAfterModbusWriteMultipleCoilsResponsePolicy3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_COILS_RESPONSE_POLICY",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleCoilsResponsePolicy3),
                EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleCoilsResponsePolicy3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusWriteMultipleCoilsResponsePolicy1 =
                positionAfterModbusWriteMultipleCoilsResponsePolicy3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusWriteMultipleCoilsResponsePolicy1))
      {
        positionAfterModbusWriteMultipleCoilsResponsePolicy0 =
          positionAfterModbusWriteMultipleCoilsResponsePolicy1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_COILS_RESPONSE_POLICY",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleCoilsResponsePolicy1),
          EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleCoilsResponsePolicy1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusWriteMultipleCoilsResponsePolicy0 =
          positionAfterModbusWriteMultipleCoilsResponsePolicy1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusWriteMultipleCoilsResponsePolicy0))
  {
    return positionAfterModbusWriteMultipleCoilsResponsePolicy0;
  }
  ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_COILS_RESPONSE_POLICY",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleCoilsResponsePolicy0),
    EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleCoilsResponsePolicy0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusWriteMultipleCoilsResponsePolicy0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusReadWriteRegistersResponsePolicy;
  if (hasBytes0)
  {
    positionAfterModbusReadWriteRegistersResponsePolicy = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusReadWriteRegistersResponsePolicy =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusReadWriteRegistersResponsePolicy))
  {
    res0 = positionAfterModbusReadWriteRegistersResponsePolicy;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_READ_WRITE_REGISTERS_RESPONSE_POLICY",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusReadWriteRegistersResponsePolicy),
      EverParseGetValidatorErrorKind(positionAfterModbusReadWriteRegistersResponsePolicy),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusReadWriteRegistersResponsePolicy;
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
  uint64_t positionAfterModbusReadWriteRegistersResponsePolicy0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusReadWriteRegistersResponsePolicy0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk =
      protocolId == (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusReadWriteRegistersResponsePolicy0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusReadWriteRegistersResponsePolicy1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusReadWriteRegistersResponsePolicy1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length >= (uint16_t)3U && length <= (uint16_t)253U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusReadWriteRegistersResponsePolicy1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusReadWriteRegistersResponsePolicy2;
          if (hasBytes3)
          {
            positionAfterModbusReadWriteRegistersResponsePolicy2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusReadWriteRegistersResponsePolicy2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusReadWriteRegistersResponsePolicy2))
          {
            res1 = positionAfterModbusReadWriteRegistersResponsePolicy2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_READ_WRITE_REGISTERS_RESPONSE_POLICY",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusReadWriteRegistersResponsePolicy2),
              EverParseGetValidatorErrorKind(positionAfterModbusReadWriteRegistersResponsePolicy2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusReadWriteRegistersResponsePolicy2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusReadWriteRegistersResponsePolicy1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusReadWriteRegistersResponsePolicy3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusReadWriteRegistersResponsePolicy3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_V5_RESPONSEPOLICY____FC_READ_WRITE_MULTIPLE_REGISTERS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusReadWriteRegistersResponsePolicy3 = positionAfterFunctionCode1;
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
                uint64_t positionAfterModbusReadWriteRegistersResponsePolicy4;
                if (EverParseIsError(positionAfterByteCount))
                {
                  positionAfterModbusReadWriteRegistersResponsePolicy4 = positionAfterByteCount;
                }
                else
                {
                  uint8_t byteCount = Input[(uint32_t)positionAfterFunctionCode1];
                  BOOLEAN
                  byteCountConstraintIsOk =
                    (uint16_t)byteCount == (uint32_t)length - (uint32_t)(uint16_t)3U &&
                      (uint32_t)byteCount % 2U == 0U
                    && byteCount <= MODBUSTCP_V5_RESPONSEPOLICY____MAX_RESPONSE_REGISTER_BYTES
                    && (uint16_t)byteCount <= MaxResponseBytes;
                  uint64_t
                  positionAfterByteCount1 =
                    EverParseCheckConstraintOk(byteCountConstraintIsOk,
                      positionAfterByteCount);
                  if (EverParseIsError(positionAfterByteCount1))
                  {
                    positionAfterModbusReadWriteRegistersResponsePolicy4 = positionAfterByteCount1;
                  }
                  else
                  {
                    /* Validating field ReadData */
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
                    uint64_t positionAfterModbusReadWriteRegistersResponsePolicy5 = res;
                    if (EverParseIsSuccess(positionAfterModbusReadWriteRegistersResponsePolicy5))
                    {
                      positionAfterModbusReadWriteRegistersResponsePolicy4 =
                        positionAfterModbusReadWriteRegistersResponsePolicy5;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_READ_WRITE_REGISTERS_RESPONSE_POLICY",
                        "ReadData",
                        EverParseErrorReasonOfResult(positionAfterModbusReadWriteRegistersResponsePolicy5),
                        EverParseGetValidatorErrorKind(positionAfterModbusReadWriteRegistersResponsePolicy5),
                        Ctxt,
                        Input,
                        positionAfterByteCount1);
                      positionAfterModbusReadWriteRegistersResponsePolicy4 =
                        positionAfterModbusReadWriteRegistersResponsePolicy5;
                    }
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusReadWriteRegistersResponsePolicy4))
                {
                  positionAfterModbusReadWriteRegistersResponsePolicy3 =
                    positionAfterModbusReadWriteRegistersResponsePolicy4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_READ_WRITE_REGISTERS_RESPONSE_POLICY",
                    "ByteCount",
                    EverParseErrorReasonOfResult(positionAfterModbusReadWriteRegistersResponsePolicy4),
                    EverParseGetValidatorErrorKind(positionAfterModbusReadWriteRegistersResponsePolicy4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusReadWriteRegistersResponsePolicy3 =
                    positionAfterModbusReadWriteRegistersResponsePolicy4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusReadWriteRegistersResponsePolicy3))
            {
              positionAfterModbusReadWriteRegistersResponsePolicy1 =
                positionAfterModbusReadWriteRegistersResponsePolicy3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_READ_WRITE_REGISTERS_RESPONSE_POLICY",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusReadWriteRegistersResponsePolicy3),
                EverParseGetValidatorErrorKind(positionAfterModbusReadWriteRegistersResponsePolicy3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusReadWriteRegistersResponsePolicy1 =
                positionAfterModbusReadWriteRegistersResponsePolicy3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusReadWriteRegistersResponsePolicy1))
      {
        positionAfterModbusReadWriteRegistersResponsePolicy0 =
          positionAfterModbusReadWriteRegistersResponsePolicy1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_READ_WRITE_REGISTERS_RESPONSE_POLICY",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusReadWriteRegistersResponsePolicy1),
          EverParseGetValidatorErrorKind(positionAfterModbusReadWriteRegistersResponsePolicy1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusReadWriteRegistersResponsePolicy0 =
          positionAfterModbusReadWriteRegistersResponsePolicy1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusReadWriteRegistersResponsePolicy0))
  {
    return positionAfterModbusReadWriteRegistersResponsePolicy0;
  }
  ErrorHandlerFn("_MODBUS_READ_WRITE_REGISTERS_RESPONSE_POLICY",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusReadWriteRegistersResponsePolicy0),
    EverParseGetValidatorErrorKind(positionAfterModbusReadWriteRegistersResponsePolicy0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusReadWriteRegistersResponsePolicy0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength0 - StartPosition);
  uint64_t positionAfterModbusExceptionResponsePolicy;
  if (hasBytes0)
  {
    positionAfterModbusExceptionResponsePolicy = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusExceptionResponsePolicy =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusExceptionResponsePolicy))
  {
    res0 = positionAfterModbusExceptionResponsePolicy;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_EXCEPTION_RESPONSE_POLICY",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusExceptionResponsePolicy),
      EverParseGetValidatorErrorKind(positionAfterModbusExceptionResponsePolicy),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusExceptionResponsePolicy;
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
  uint64_t positionAfterModbusExceptionResponsePolicy0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusExceptionResponsePolicy0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk =
      protocolId == (uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusExceptionResponsePolicy0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusExceptionResponsePolicy1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusExceptionResponsePolicy1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN
        lengthConstraintIsOk =
          length == (uint16_t)3U &&
            InputLength ==
              (uint32_t)((uint32_t)length +
                (uint32_t)(uint16_t)MODBUSTCP_V5_RESPONSEPOLICY____MBAP_HEADER_PREFIX_SIZE);
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusExceptionResponsePolicy1 = positionAfterLength1;
        }
        else
        {
          /*  Exception FC must be valid (original FC | 0x80) */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength0 - positionAfterLength1);
          uint64_t positionAfterModbusExceptionResponsePolicy2;
          if (hasBytes3)
          {
            positionAfterModbusExceptionResponsePolicy2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusExceptionResponsePolicy2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res;
          if (EverParseIsSuccess(positionAfterModbusExceptionResponsePolicy2))
          {
            res = positionAfterModbusExceptionResponsePolicy2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_EXCEPTION_RESPONSE_POLICY",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusExceptionResponsePolicy2),
              EverParseGetValidatorErrorKind(positionAfterModbusExceptionResponsePolicy2),
              Ctxt,
              Input,
              positionAfterLength1);
            res = positionAfterModbusExceptionResponsePolicy2;
          }
          uint64_t positionAfterUnitId = res;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusExceptionResponsePolicy1 = positionAfterUnitId;
          }
          else
          {
            /* Checking that we have enough space for a UINT8, i.e., 1 byte */
            BOOLEAN hasBytes4 = 1ULL <= (InputLength0 - positionAfterUnitId);
            uint64_t positionAfterExceptionFunctionCode;
            if (hasBytes4)
            {
              positionAfterExceptionFunctionCode = positionAfterUnitId + 1ULL;
            }
            else
            {
              positionAfterExceptionFunctionCode =
                EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                  positionAfterUnitId);
            }
            uint64_t positionAfterModbusExceptionResponsePolicy3;
            if (EverParseIsError(positionAfterExceptionFunctionCode))
            {
              positionAfterModbusExceptionResponsePolicy3 = positionAfterExceptionFunctionCode;
            }
            else
            {
              uint8_t exceptionFunctionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              exceptionFunctionCodeConstraintIsOk =
                exceptionFunctionCode == MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_READ_COILS ||
                  exceptionFunctionCode ==
                    MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_READ_DISCRETE_INPUTS
                ||
                  exceptionFunctionCode ==
                    MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_READ_HOLDING_REGISTERS
                ||
                  exceptionFunctionCode ==
                    MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_READ_INPUT_REGISTERS
                ||
                  exceptionFunctionCode ==
                    MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_WRITE_SINGLE_COIL
                ||
                  exceptionFunctionCode ==
                    MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_WRITE_SINGLE_REGISTER
                ||
                  exceptionFunctionCode ==
                    MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_WRITE_MULTIPLE_COILS
                ||
                  exceptionFunctionCode ==
                    MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_WRITE_MULTIPLE_REGISTERS
                ||
                  exceptionFunctionCode ==
                    MODBUSTCP_V5_RESPONSEPOLICY____FC_EXCEPTION_READ_WRITE_MULTIPLE_REGISTERS;
              uint64_t
              positionAfterExceptionFunctionCode1 =
                EverParseCheckConstraintOk(exceptionFunctionCodeConstraintIsOk,
                  positionAfterExceptionFunctionCode);
              if (EverParseIsError(positionAfterExceptionFunctionCode1))
              {
                positionAfterModbusExceptionResponsePolicy3 = positionAfterExceptionFunctionCode1;
              }
              else
              {
                /* Validating field ExceptionCode */
                /* Checking that we have enough space for a UINT8, i.e., 1 byte */
                BOOLEAN hasBytes = 1ULL <= (InputLength0 - positionAfterExceptionFunctionCode1);
                uint64_t positionAfterExceptionCode_refinement;
                if (hasBytes)
                {
                  positionAfterExceptionCode_refinement = positionAfterExceptionFunctionCode1 + 1ULL;
                }
                else
                {
                  positionAfterExceptionCode_refinement =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterExceptionFunctionCode1);
                }
                uint64_t positionAfterModbusExceptionResponsePolicy4;
                if (EverParseIsError(positionAfterExceptionCode_refinement))
                {
                  positionAfterModbusExceptionResponsePolicy4 =
                    positionAfterExceptionCode_refinement;
                }
                else
                {
                  /* reading field_value */
                  uint8_t
                  exceptionCode_refinement = Input[(uint32_t)positionAfterExceptionFunctionCode1];
                  /* start: checking constraint */
                  BOOLEAN
                  exceptionCode_refinementConstraintIsOk =
                    exceptionCode_refinement ==
                      MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_ILLEGAL_FUNCTION
                    ||
                      exceptionCode_refinement ==
                        MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_ILLEGAL_DATA_ADDRESS
                    ||
                      exceptionCode_refinement ==
                        MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_ILLEGAL_DATA_VALUE
                    ||
                      exceptionCode_refinement ==
                        MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_SERVER_DEVICE_FAILURE
                    ||
                      exceptionCode_refinement ==
                        MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_ACKNOWLEDGE
                    ||
                      exceptionCode_refinement ==
                        MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_SERVER_DEVICE_BUSY
                    ||
                      exceptionCode_refinement ==
                        MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_MEMORY_PARITY_ERROR
                    ||
                      exceptionCode_refinement ==
                        MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_GATEWAY_PATH_UNAVAIL
                    ||
                      exceptionCode_refinement ==
                        MODBUSTCP_V5_RESPONSEPOLICY____EXCEPTION_GATEWAY_TARGET_FAILED;
                  /* end: checking constraint */
                  positionAfterModbusExceptionResponsePolicy4 =
                    EverParseCheckConstraintOk(exceptionCode_refinementConstraintIsOk,
                      positionAfterExceptionCode_refinement);
                }
                if (EverParseIsSuccess(positionAfterModbusExceptionResponsePolicy4))
                {
                  positionAfterModbusExceptionResponsePolicy3 =
                    positionAfterModbusExceptionResponsePolicy4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_EXCEPTION_RESPONSE_POLICY",
                    "ExceptionCode.refinement",
                    EverParseErrorReasonOfResult(positionAfterModbusExceptionResponsePolicy4),
                    EverParseGetValidatorErrorKind(positionAfterModbusExceptionResponsePolicy4),
                    Ctxt,
                    Input,
                    positionAfterExceptionFunctionCode1);
                  positionAfterModbusExceptionResponsePolicy3 =
                    positionAfterModbusExceptionResponsePolicy4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusExceptionResponsePolicy3))
            {
              positionAfterModbusExceptionResponsePolicy1 =
                positionAfterModbusExceptionResponsePolicy3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_EXCEPTION_RESPONSE_POLICY",
                "ExceptionFunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusExceptionResponsePolicy3),
                EverParseGetValidatorErrorKind(positionAfterModbusExceptionResponsePolicy3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusExceptionResponsePolicy1 =
                positionAfterModbusExceptionResponsePolicy3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusExceptionResponsePolicy1))
      {
        positionAfterModbusExceptionResponsePolicy0 = positionAfterModbusExceptionResponsePolicy1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_EXCEPTION_RESPONSE_POLICY",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusExceptionResponsePolicy1),
          EverParseGetValidatorErrorKind(positionAfterModbusExceptionResponsePolicy1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusExceptionResponsePolicy0 = positionAfterModbusExceptionResponsePolicy1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusExceptionResponsePolicy0))
  {
    return positionAfterModbusExceptionResponsePolicy0;
  }
  ErrorHandlerFn("_MODBUS_EXCEPTION_RESPONSE_POLICY",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusExceptionResponsePolicy0),
    EverParseGetValidatorErrorKind(positionAfterModbusExceptionResponsePolicy0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusExceptionResponsePolicy0;
}

