#include "ModbusTCP_v5_ResponsePolicyWrapper.h"
#include "EverParse.h"
#include "ModbusTCP_v5_ResponsePolicy.h"
void ModbusTCP_v5_ResponsePolicyEverParseError(const char *StructName, const char *FieldName, const char *Reason);

static
void DefaultErrorHandler(
	const char *typename_s,
	const char *fieldname,
	const char *reason,
	uint64_t error_code,
	uint8_t *context,
	EVERPARSE_INPUT_BUFFER input,
	uint64_t start_pos)
{
	EVERPARSE_ERROR_FRAME *frame = (EVERPARSE_ERROR_FRAME*)context;
	EverParseDefaultErrorHandler(
		typename_s,
		fieldname,
		reason,
		error_code,
		frame,
		input,
		start_pos
	);
}

BOOLEAN ModbusTcpV5ResponsePolicyCheckModbusReadHoldingRegistersResponsePolicy(uint32_t ___InputLength, uint16_t ___MaxResponseBytes, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV5ResponsePolicyValidateModbusReadHoldingRegistersResponsePolicy(___InputLength, ___MaxResponseBytes,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v5_ResponsePolicyEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV5ResponsePolicyCheckModbusReadInputRegistersResponsePolicy(uint32_t ___InputLength, uint16_t ___MaxResponseBytes, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV5ResponsePolicyValidateModbusReadInputRegistersResponsePolicy(___InputLength, ___MaxResponseBytes,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v5_ResponsePolicyEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV5ResponsePolicyCheckModbusReadCoilsResponsePolicy(uint32_t ___InputLength, uint16_t ___MaxResponseBytes, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV5ResponsePolicyValidateModbusReadCoilsResponsePolicy(___InputLength, ___MaxResponseBytes,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v5_ResponsePolicyEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV5ResponsePolicyCheckModbusReadDiscreteInputsResponsePolicy(uint32_t ___InputLength, uint16_t ___MaxResponseBytes, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV5ResponsePolicyValidateModbusReadDiscreteInputsResponsePolicy(___InputLength, ___MaxResponseBytes,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v5_ResponsePolicyEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV5ResponsePolicyCheckModbusWriteSingleRegisterResponsePolicy(uint32_t ___InputLength, uint16_t ___AllowedStart, uint16_t ___AllowedEnd, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV5ResponsePolicyValidateModbusWriteSingleRegisterResponsePolicy(___InputLength, ___AllowedStart, ___AllowedEnd,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v5_ResponsePolicyEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV5ResponsePolicyCheckModbusWriteSingleCoilResponsePolicy(uint32_t ___InputLength, uint16_t ___AllowedStart, uint16_t ___AllowedEnd, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV5ResponsePolicyValidateModbusWriteSingleCoilResponsePolicy(___InputLength, ___AllowedStart, ___AllowedEnd,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v5_ResponsePolicyEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV5ResponsePolicyCheckModbusWriteMultipleRegistersResponsePolicy(uint32_t ___InputLength, uint16_t ___AllowedStart, uint16_t ___AllowedEnd, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV5ResponsePolicyValidateModbusWriteMultipleRegistersResponsePolicy(___InputLength, ___AllowedStart, ___AllowedEnd,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v5_ResponsePolicyEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV5ResponsePolicyCheckModbusWriteMultipleCoilsResponsePolicy(uint32_t ___InputLength, uint16_t ___AllowedStart, uint16_t ___AllowedEnd, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV5ResponsePolicyValidateModbusWriteMultipleCoilsResponsePolicy(___InputLength, ___AllowedStart, ___AllowedEnd,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v5_ResponsePolicyEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV5ResponsePolicyCheckModbusReadWriteRegistersResponsePolicy(uint32_t ___InputLength, uint16_t ___MaxResponseBytes, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV5ResponsePolicyValidateModbusReadWriteRegistersResponsePolicy(___InputLength, ___MaxResponseBytes,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v5_ResponsePolicyEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV5ResponsePolicyCheckModbusExceptionResponsePolicy(uint32_t ___InputLength, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV5ResponsePolicyValidateModbusExceptionResponsePolicy(___InputLength,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v5_ResponsePolicyEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}
