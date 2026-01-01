#include "ModbusTCP_v3_SimpleWrapper.h"
#include "EverParse.h"
#include "ModbusTCP_v3_Simple.h"
void ModbusTCP_v3_SimpleEverParseError(const char *StructName, const char *FieldName, const char *Reason);

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

BOOLEAN ModbusTcpV3SimpleCheckModbusTcpFrameV3(uint32_t ___InputLength, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV3SimpleValidateModbusTcpFrameV3(___InputLength,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v3_SimpleEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV3SimpleCheckModbusReadRequestV3(uint32_t ___InputLength, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV3SimpleValidateModbusReadRequestV3(___InputLength,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v3_SimpleEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV3SimpleCheckModbusWriteSingleRequestV3(uint32_t ___InputLength, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV3SimpleValidateModbusWriteSingleRequestV3(___InputLength,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v3_SimpleEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV3SimpleCheckModbusWriteMultipleRequestV3(uint32_t ___InputLength, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV3SimpleValidateModbusWriteMultipleRequestV3(___InputLength,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v3_SimpleEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV3SimpleCheckModbusReadResponseV3(uint32_t ___InputLength, uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV3SimpleValidateModbusReadResponseV3(___InputLength,  (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v3_SimpleEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}

BOOLEAN ModbusTcpV3SimpleCheckModbusTcpFrameUnsafe(uint8_t *base, uint32_t len) {
	EVERPARSE_ERROR_FRAME frame;
	frame.filled = FALSE;
	uint64_t result = ModbusTcpV3SimpleValidateModbusTcpFrameUnsafe( (uint8_t*)&frame, &DefaultErrorHandler, base, len, 0);
	if (EverParseIsError(result))
	{
		if (frame.filled)
		{
			ModbusTCP_v3_SimpleEverParseError(frame.typename_s, frame.fieldname, frame.reason);
		}
		return FALSE;
	}
	return TRUE;
}
