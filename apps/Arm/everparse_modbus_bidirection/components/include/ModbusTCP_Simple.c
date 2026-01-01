

#include "ModbusTCP_Simple.h"

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
)
{
  /*  MBAP Header (7 bytes) */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength - StartPosition);
  uint64_t positionAfterModbusTcpFrame;
  if (hasBytes0)
  {
    positionAfterModbusTcpFrame = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusTcpFrame =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusTcpFrame))
  {
    res0 = positionAfterModbusTcpFrame;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_TCP_FRAME",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusTcpFrame),
      EverParseGetValidatorErrorKind(positionAfterModbusTcpFrame),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusTcpFrame;
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
  uint64_t positionAfterModbusTcpFrame0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusTcpFrame0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusTcpFrame0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusTcpFrame1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusTcpFrame1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN lengthConstraintIsOk = length >= (uint16_t)2U && length <= (uint16_t)254U;
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusTcpFrame1 = positionAfterLength1;
        }
        else
        {
          /*  PDU starts here */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength - positionAfterLength1);
          uint64_t positionAfterModbusTcpFrame2;
          if (hasBytes3)
          {
            positionAfterModbusTcpFrame2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusTcpFrame2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusTcpFrame2))
          {
            res1 = positionAfterModbusTcpFrame2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_TCP_FRAME",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusTcpFrame2),
              EverParseGetValidatorErrorKind(positionAfterModbusTcpFrame2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusTcpFrame2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusTcpFrame1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusTcpFrame3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusTcpFrame3 = positionAfterFunctionCode;
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
                positionAfterModbusTcpFrame3 = positionAfterFunctionCode1;
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
                uint64_t positionAfterModbusTcpFrame4 = res;
                if (EverParseIsSuccess(positionAfterModbusTcpFrame4))
                {
                  positionAfterModbusTcpFrame3 = positionAfterModbusTcpFrame4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_TCP_FRAME",
                    "PDUData",
                    EverParseErrorReasonOfResult(positionAfterModbusTcpFrame4),
                    EverParseGetValidatorErrorKind(positionAfterModbusTcpFrame4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusTcpFrame3 = positionAfterModbusTcpFrame4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusTcpFrame3))
            {
              positionAfterModbusTcpFrame1 = positionAfterModbusTcpFrame3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_TCP_FRAME",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusTcpFrame3),
                EverParseGetValidatorErrorKind(positionAfterModbusTcpFrame3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusTcpFrame1 = positionAfterModbusTcpFrame3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusTcpFrame1))
      {
        positionAfterModbusTcpFrame0 = positionAfterModbusTcpFrame1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_TCP_FRAME",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusTcpFrame1),
          EverParseGetValidatorErrorKind(positionAfterModbusTcpFrame1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusTcpFrame0 = positionAfterModbusTcpFrame1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusTcpFrame0))
  {
    return positionAfterModbusTcpFrame0;
  }
  ErrorHandlerFn("_MODBUS_TCP_FRAME",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusTcpFrame0),
    EverParseGetValidatorErrorKind(positionAfterModbusTcpFrame0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusTcpFrame0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength - StartPosition);
  uint64_t positionAfterModbusReadRequest;
  if (hasBytes0)
  {
    positionAfterModbusReadRequest = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusReadRequest =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusReadRequest))
  {
    res0 = positionAfterModbusReadRequest;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_READ_REQUEST",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusReadRequest),
      EverParseGetValidatorErrorKind(positionAfterModbusReadRequest),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusReadRequest;
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
  uint64_t positionAfterModbusReadRequest0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusReadRequest0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusReadRequest0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusReadRequest1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusReadRequest1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN lengthConstraintIsOk = length == (uint16_t)6U;
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusReadRequest1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength - positionAfterLength1);
          uint64_t positionAfterModbusReadRequest2;
          if (hasBytes3)
          {
            positionAfterModbusReadRequest2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusReadRequest2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusReadRequest2))
          {
            res1 = positionAfterModbusReadRequest2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_READ_REQUEST",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusReadRequest2),
              EverParseGetValidatorErrorKind(positionAfterModbusReadRequest2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusReadRequest2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusReadRequest1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusReadRequest3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusReadRequest3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_SIMPLE____FC_READ_COILS ||
                  functionCode == MODBUSTCP_SIMPLE____FC_READ_HOLDING_REGISTERS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusReadRequest3 = positionAfterFunctionCode1;
              }
              else
              {
                /* Validating field StartAddress */
                /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                BOOLEAN hasBytes5 = 2ULL <= (InputLength - positionAfterFunctionCode1);
                uint64_t positionAfterModbusReadRequest4;
                if (hasBytes5)
                {
                  positionAfterModbusReadRequest4 = positionAfterFunctionCode1 + 2ULL;
                }
                else
                {
                  positionAfterModbusReadRequest4 =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t res;
                if (EverParseIsSuccess(positionAfterModbusReadRequest4))
                {
                  res = positionAfterModbusReadRequest4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_READ_REQUEST",
                    "StartAddress",
                    EverParseErrorReasonOfResult(positionAfterModbusReadRequest4),
                    EverParseGetValidatorErrorKind(positionAfterModbusReadRequest4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  res = positionAfterModbusReadRequest4;
                }
                uint64_t positionAfterStartAddress = res;
                if (EverParseIsError(positionAfterStartAddress))
                {
                  positionAfterModbusReadRequest3 = positionAfterStartAddress;
                }
                else
                {
                  /* Validating field Quantity */
                  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                  BOOLEAN hasBytes = 2ULL <= (InputLength - positionAfterStartAddress);
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
                  uint64_t positionAfterModbusReadRequest5;
                  if (EverParseIsError(positionAfterQuantity_refinement))
                  {
                    positionAfterModbusReadRequest5 = positionAfterQuantity_refinement;
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
                    positionAfterModbusReadRequest5 =
                      EverParseCheckConstraintOk(quantity_refinementConstraintIsOk,
                        positionAfterQuantity_refinement);
                  }
                  if (EverParseIsSuccess(positionAfterModbusReadRequest5))
                  {
                    positionAfterModbusReadRequest3 = positionAfterModbusReadRequest5;
                  }
                  else
                  {
                    ErrorHandlerFn("_MODBUS_READ_REQUEST",
                      "Quantity.refinement",
                      EverParseErrorReasonOfResult(positionAfterModbusReadRequest5),
                      EverParseGetValidatorErrorKind(positionAfterModbusReadRequest5),
                      Ctxt,
                      Input,
                      positionAfterStartAddress);
                    positionAfterModbusReadRequest3 = positionAfterModbusReadRequest5;
                  }
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusReadRequest3))
            {
              positionAfterModbusReadRequest1 = positionAfterModbusReadRequest3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_READ_REQUEST",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusReadRequest3),
                EverParseGetValidatorErrorKind(positionAfterModbusReadRequest3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusReadRequest1 = positionAfterModbusReadRequest3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusReadRequest1))
      {
        positionAfterModbusReadRequest0 = positionAfterModbusReadRequest1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_READ_REQUEST",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusReadRequest1),
          EverParseGetValidatorErrorKind(positionAfterModbusReadRequest1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusReadRequest0 = positionAfterModbusReadRequest1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusReadRequest0))
  {
    return positionAfterModbusReadRequest0;
  }
  ErrorHandlerFn("_MODBUS_READ_REQUEST",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusReadRequest0),
    EverParseGetValidatorErrorKind(positionAfterModbusReadRequest0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusReadRequest0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength - StartPosition);
  uint64_t positionAfterModbusWriteSingleRequest;
  if (hasBytes0)
  {
    positionAfterModbusWriteSingleRequest = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusWriteSingleRequest =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusWriteSingleRequest))
  {
    res0 = positionAfterModbusWriteSingleRequest;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REQUEST",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRequest),
      EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRequest),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusWriteSingleRequest;
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
  uint64_t positionAfterModbusWriteSingleRequest0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusWriteSingleRequest0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusWriteSingleRequest0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusWriteSingleRequest1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusWriteSingleRequest1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN lengthConstraintIsOk = length == (uint16_t)6U;
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusWriteSingleRequest1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength - positionAfterLength1);
          uint64_t positionAfterModbusWriteSingleRequest2;
          if (hasBytes3)
          {
            positionAfterModbusWriteSingleRequest2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusWriteSingleRequest2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusWriteSingleRequest2))
          {
            res1 = positionAfterModbusWriteSingleRequest2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REQUEST",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRequest2),
              EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRequest2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusWriteSingleRequest2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusWriteSingleRequest1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusWriteSingleRequest3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusWriteSingleRequest3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_SIMPLE____FC_WRITE_SINGLE_REGISTER;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusWriteSingleRequest3 = positionAfterFunctionCode1;
              }
              else
              {
                BOOLEAN hasBytes = 4ULL <= (InputLength - positionAfterFunctionCode1);
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
                positionAfterModbusWriteSingleRequest3 = res;
              }
            }
            if (EverParseIsSuccess(positionAfterModbusWriteSingleRequest3))
            {
              positionAfterModbusWriteSingleRequest1 = positionAfterModbusWriteSingleRequest3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REQUEST",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRequest3),
                EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRequest3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusWriteSingleRequest1 = positionAfterModbusWriteSingleRequest3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusWriteSingleRequest1))
      {
        positionAfterModbusWriteSingleRequest0 = positionAfterModbusWriteSingleRequest1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REQUEST",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRequest1),
          EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRequest1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusWriteSingleRequest0 = positionAfterModbusWriteSingleRequest1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusWriteSingleRequest0))
  {
    return positionAfterModbusWriteSingleRequest0;
  }
  ErrorHandlerFn("_MODBUS_WRITE_SINGLE_REQUEST",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusWriteSingleRequest0),
    EverParseGetValidatorErrorKind(positionAfterModbusWriteSingleRequest0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusWriteSingleRequest0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength - StartPosition);
  uint64_t positionAfterModbusWriteMultipleRequest;
  if (hasBytes0)
  {
    positionAfterModbusWriteMultipleRequest = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusWriteMultipleRequest =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequest))
  {
    res0 = positionAfterModbusWriteMultipleRequest;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequest),
      EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequest),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusWriteMultipleRequest;
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
  uint64_t positionAfterModbusWriteMultipleRequest0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusWriteMultipleRequest0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusWriteMultipleRequest0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusWriteMultipleRequest1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusWriteMultipleRequest1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN lengthConstraintIsOk = length >= (uint16_t)9U && length <= (uint16_t)253U;
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusWriteMultipleRequest1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength - positionAfterLength1);
          uint64_t positionAfterModbusWriteMultipleRequest2;
          if (hasBytes3)
          {
            positionAfterModbusWriteMultipleRequest2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusWriteMultipleRequest2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequest2))
          {
            res1 = positionAfterModbusWriteMultipleRequest2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequest2),
              EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequest2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusWriteMultipleRequest2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusWriteMultipleRequest1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusWriteMultipleRequest3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusWriteMultipleRequest3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_SIMPLE____FC_WRITE_MULTIPLE_REGISTERS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusWriteMultipleRequest3 = positionAfterFunctionCode1;
              }
              else
              {
                /* Validating field StartAddress */
                /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                BOOLEAN hasBytes5 = 2ULL <= (InputLength - positionAfterFunctionCode1);
                uint64_t positionAfterModbusWriteMultipleRequest4;
                if (hasBytes5)
                {
                  positionAfterModbusWriteMultipleRequest4 = positionAfterFunctionCode1 + 2ULL;
                }
                else
                {
                  positionAfterModbusWriteMultipleRequest4 =
                    EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                      positionAfterFunctionCode1);
                }
                uint64_t res2;
                if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequest4))
                {
                  res2 = positionAfterModbusWriteMultipleRequest4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST",
                    "StartAddress",
                    EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequest4),
                    EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequest4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  res2 = positionAfterModbusWriteMultipleRequest4;
                }
                uint64_t positionAfterStartAddress = res2;
                if (EverParseIsError(positionAfterStartAddress))
                {
                  positionAfterModbusWriteMultipleRequest3 = positionAfterStartAddress;
                }
                else
                {
                  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
                  BOOLEAN hasBytes6 = 2ULL <= (InputLength - positionAfterStartAddress);
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
                  uint64_t positionAfterModbusWriteMultipleRequest5;
                  if (EverParseIsError(positionAfterQuantity))
                  {
                    positionAfterModbusWriteMultipleRequest5 = positionAfterQuantity;
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
                      positionAfterModbusWriteMultipleRequest5 = positionAfterQuantity1;
                    }
                    else
                    {
                      /* Checking that we have enough space for a UINT8, i.e., 1 byte */
                      BOOLEAN hasBytes7 = 1ULL <= (InputLength - positionAfterQuantity1);
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
                      uint64_t positionAfterModbusWriteMultipleRequest6;
                      if (EverParseIsError(positionAfterByteCount))
                      {
                        positionAfterModbusWriteMultipleRequest6 = positionAfterByteCount;
                      }
                      else
                      {
                        uint8_t byteCount = Input[(uint32_t)positionAfterQuantity1];
                        BOOLEAN
                        byteCountConstraintIsOk =
                          (uint16_t)byteCount == (uint32_t)quantity * (uint32_t)(uint16_t)2U;
                        uint64_t
                        positionAfterByteCount1 =
                          EverParseCheckConstraintOk(byteCountConstraintIsOk,
                            positionAfterByteCount);
                        if (EverParseIsError(positionAfterByteCount1))
                        {
                          positionAfterModbusWriteMultipleRequest6 = positionAfterByteCount1;
                        }
                        else
                        {
                          /* Validating field RegisterValues */
                          BOOLEAN
                          hasBytes =
                            (uint64_t)(uint32_t)byteCount <= (InputLength - positionAfterByteCount1);
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
                          uint64_t positionAfterModbusWriteMultipleRequest7 = res;
                          if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequest7))
                          {
                            positionAfterModbusWriteMultipleRequest6 =
                              positionAfterModbusWriteMultipleRequest7;
                          }
                          else
                          {
                            ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST",
                              "RegisterValues",
                              EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequest7),
                              EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequest7),
                              Ctxt,
                              Input,
                              positionAfterByteCount1);
                            positionAfterModbusWriteMultipleRequest6 =
                              positionAfterModbusWriteMultipleRequest7;
                          }
                        }
                      }
                      if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequest6))
                      {
                        positionAfterModbusWriteMultipleRequest5 =
                          positionAfterModbusWriteMultipleRequest6;
                      }
                      else
                      {
                        ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST",
                          "ByteCount",
                          EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequest6),
                          EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequest6),
                          Ctxt,
                          Input,
                          positionAfterQuantity1);
                        positionAfterModbusWriteMultipleRequest5 =
                          positionAfterModbusWriteMultipleRequest6;
                      }
                    }
                  }
                  if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequest5))
                  {
                    positionAfterModbusWriteMultipleRequest3 =
                      positionAfterModbusWriteMultipleRequest5;
                  }
                  else
                  {
                    ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST",
                      "Quantity",
                      EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequest5),
                      EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequest5),
                      Ctxt,
                      Input,
                      positionAfterStartAddress);
                    positionAfterModbusWriteMultipleRequest3 =
                      positionAfterModbusWriteMultipleRequest5;
                  }
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequest3))
            {
              positionAfterModbusWriteMultipleRequest1 = positionAfterModbusWriteMultipleRequest3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequest3),
                EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequest3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusWriteMultipleRequest1 = positionAfterModbusWriteMultipleRequest3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequest1))
      {
        positionAfterModbusWriteMultipleRequest0 = positionAfterModbusWriteMultipleRequest1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequest1),
          EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequest1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusWriteMultipleRequest0 = positionAfterModbusWriteMultipleRequest1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusWriteMultipleRequest0))
  {
    return positionAfterModbusWriteMultipleRequest0;
  }
  ErrorHandlerFn("_MODBUS_WRITE_MULTIPLE_REQUEST",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusWriteMultipleRequest0),
    EverParseGetValidatorErrorKind(positionAfterModbusWriteMultipleRequest0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusWriteMultipleRequest0;
}

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
)
{
  /* Validating field TransactionId */
  /* Checking that we have enough space for a UINT16BE, i.e., 2 bytes */
  BOOLEAN hasBytes0 = 2ULL <= (InputLength - StartPosition);
  uint64_t positionAfterModbusReadResponse;
  if (hasBytes0)
  {
    positionAfterModbusReadResponse = StartPosition + 2ULL;
  }
  else
  {
    positionAfterModbusReadResponse =
      EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
        StartPosition);
  }
  uint64_t res0;
  if (EverParseIsSuccess(positionAfterModbusReadResponse))
  {
    res0 = positionAfterModbusReadResponse;
  }
  else
  {
    ErrorHandlerFn("_MODBUS_READ_RESPONSE",
      "TransactionId",
      EverParseErrorReasonOfResult(positionAfterModbusReadResponse),
      EverParseGetValidatorErrorKind(positionAfterModbusReadResponse),
      Ctxt,
      Input,
      StartPosition);
    res0 = positionAfterModbusReadResponse;
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
  uint64_t positionAfterModbusReadResponse0;
  if (EverParseIsError(positionAfterProtocolId))
  {
    positionAfterModbusReadResponse0 = positionAfterProtocolId;
  }
  else
  {
    uint16_t protocolId = Load16Be(Input + (uint32_t)positionAfterTransactionId);
    BOOLEAN
    protocolIdConstraintIsOk = protocolId == (uint16_t)MODBUSTCP_SIMPLE____MODBUS_PROTOCOL_ID;
    uint64_t
    positionAfterProtocolId1 =
      EverParseCheckConstraintOk(protocolIdConstraintIsOk,
        positionAfterProtocolId);
    if (EverParseIsError(positionAfterProtocolId1))
    {
      positionAfterModbusReadResponse0 = positionAfterProtocolId1;
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
      uint64_t positionAfterModbusReadResponse1;
      if (EverParseIsError(positionAfterLength))
      {
        positionAfterModbusReadResponse1 = positionAfterLength;
      }
      else
      {
        uint16_t length = Load16Be(Input + (uint32_t)positionAfterProtocolId1);
        BOOLEAN lengthConstraintIsOk = length >= (uint16_t)3U && length <= (uint16_t)253U;
        uint64_t
        positionAfterLength1 = EverParseCheckConstraintOk(lengthConstraintIsOk, positionAfterLength);
        if (EverParseIsError(positionAfterLength1))
        {
          positionAfterModbusReadResponse1 = positionAfterLength1;
        }
        else
        {
          /* Validating field UnitId */
          /* Checking that we have enough space for a UINT8, i.e., 1 byte */
          BOOLEAN hasBytes3 = 1ULL <= (InputLength - positionAfterLength1);
          uint64_t positionAfterModbusReadResponse2;
          if (hasBytes3)
          {
            positionAfterModbusReadResponse2 = positionAfterLength1 + 1ULL;
          }
          else
          {
            positionAfterModbusReadResponse2 =
              EverParseSetValidatorErrorPos(EVERPARSE_VALIDATOR_ERROR_NOT_ENOUGH_DATA,
                positionAfterLength1);
          }
          uint64_t res1;
          if (EverParseIsSuccess(positionAfterModbusReadResponse2))
          {
            res1 = positionAfterModbusReadResponse2;
          }
          else
          {
            ErrorHandlerFn("_MODBUS_READ_RESPONSE",
              "UnitId",
              EverParseErrorReasonOfResult(positionAfterModbusReadResponse2),
              EverParseGetValidatorErrorKind(positionAfterModbusReadResponse2),
              Ctxt,
              Input,
              positionAfterLength1);
            res1 = positionAfterModbusReadResponse2;
          }
          uint64_t positionAfterUnitId = res1;
          if (EverParseIsError(positionAfterUnitId))
          {
            positionAfterModbusReadResponse1 = positionAfterUnitId;
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
            uint64_t positionAfterModbusReadResponse3;
            if (EverParseIsError(positionAfterFunctionCode))
            {
              positionAfterModbusReadResponse3 = positionAfterFunctionCode;
            }
            else
            {
              uint8_t functionCode = Input[(uint32_t)positionAfterUnitId];
              BOOLEAN
              functionCodeConstraintIsOk =
                functionCode == MODBUSTCP_SIMPLE____FC_READ_COILS ||
                  functionCode == MODBUSTCP_SIMPLE____FC_READ_HOLDING_REGISTERS;
              uint64_t
              positionAfterFunctionCode1 =
                EverParseCheckConstraintOk(functionCodeConstraintIsOk,
                  positionAfterFunctionCode);
              if (EverParseIsError(positionAfterFunctionCode1))
              {
                positionAfterModbusReadResponse3 = positionAfterFunctionCode1;
              }
              else
              {
                /* Checking that we have enough space for a UINT8, i.e., 1 byte */
                BOOLEAN hasBytes5 = 1ULL <= (InputLength - positionAfterFunctionCode1);
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
                uint64_t positionAfterModbusReadResponse4;
                if (EverParseIsError(positionAfterByteCount))
                {
                  positionAfterModbusReadResponse4 = positionAfterByteCount;
                }
                else
                {
                  uint8_t byteCount = Input[(uint32_t)positionAfterFunctionCode1];
                  BOOLEAN byteCountConstraintIsOk = byteCount >= 1U && byteCount <= 250U;
                  uint64_t
                  positionAfterByteCount1 =
                    EverParseCheckConstraintOk(byteCountConstraintIsOk,
                      positionAfterByteCount);
                  if (EverParseIsError(positionAfterByteCount1))
                  {
                    positionAfterModbusReadResponse4 = positionAfterByteCount1;
                  }
                  else
                  {
                    /* Validating field Data */
                    BOOLEAN
                    hasBytes =
                      (uint64_t)(uint32_t)byteCount <= (InputLength - positionAfterByteCount1);
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
                    uint64_t positionAfterModbusReadResponse5 = res;
                    if (EverParseIsSuccess(positionAfterModbusReadResponse5))
                    {
                      positionAfterModbusReadResponse4 = positionAfterModbusReadResponse5;
                    }
                    else
                    {
                      ErrorHandlerFn("_MODBUS_READ_RESPONSE",
                        "Data",
                        EverParseErrorReasonOfResult(positionAfterModbusReadResponse5),
                        EverParseGetValidatorErrorKind(positionAfterModbusReadResponse5),
                        Ctxt,
                        Input,
                        positionAfterByteCount1);
                      positionAfterModbusReadResponse4 = positionAfterModbusReadResponse5;
                    }
                  }
                }
                if (EverParseIsSuccess(positionAfterModbusReadResponse4))
                {
                  positionAfterModbusReadResponse3 = positionAfterModbusReadResponse4;
                }
                else
                {
                  ErrorHandlerFn("_MODBUS_READ_RESPONSE",
                    "ByteCount",
                    EverParseErrorReasonOfResult(positionAfterModbusReadResponse4),
                    EverParseGetValidatorErrorKind(positionAfterModbusReadResponse4),
                    Ctxt,
                    Input,
                    positionAfterFunctionCode1);
                  positionAfterModbusReadResponse3 = positionAfterModbusReadResponse4;
                }
              }
            }
            if (EverParseIsSuccess(positionAfterModbusReadResponse3))
            {
              positionAfterModbusReadResponse1 = positionAfterModbusReadResponse3;
            }
            else
            {
              ErrorHandlerFn("_MODBUS_READ_RESPONSE",
                "FunctionCode",
                EverParseErrorReasonOfResult(positionAfterModbusReadResponse3),
                EverParseGetValidatorErrorKind(positionAfterModbusReadResponse3),
                Ctxt,
                Input,
                positionAfterUnitId);
              positionAfterModbusReadResponse1 = positionAfterModbusReadResponse3;
            }
          }
        }
      }
      if (EverParseIsSuccess(positionAfterModbusReadResponse1))
      {
        positionAfterModbusReadResponse0 = positionAfterModbusReadResponse1;
      }
      else
      {
        ErrorHandlerFn("_MODBUS_READ_RESPONSE",
          "Length",
          EverParseErrorReasonOfResult(positionAfterModbusReadResponse1),
          EverParseGetValidatorErrorKind(positionAfterModbusReadResponse1),
          Ctxt,
          Input,
          positionAfterProtocolId1);
        positionAfterModbusReadResponse0 = positionAfterModbusReadResponse1;
      }
    }
  }
  if (EverParseIsSuccess(positionAfterModbusReadResponse0))
  {
    return positionAfterModbusReadResponse0;
  }
  ErrorHandlerFn("_MODBUS_READ_RESPONSE",
    "ProtocolId",
    EverParseErrorReasonOfResult(positionAfterModbusReadResponse0),
    EverParseGetValidatorErrorKind(positionAfterModbusReadResponse0),
    Ctxt,
    Input,
    positionAfterTransactionId);
  return positionAfterModbusReadResponse0;
}

