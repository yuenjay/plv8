/*-------------------------------------------------------------------------
 *
 * plv8_type.cc : Postgres from/to v8 data converters.
 *
 * Copyright (c) 2009-2012, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#include "plv8.h"

extern "C" {
#if JSONB_DIRECT_CONVERSION
#include <time.h>
#endif
#if PG_VERSION_NUM >= 90300
#include "access/htup_details.h"
#endif
#include "catalog/pg_type.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/builtins.h"
#if PG_VERSION_NUM >= 90400
#include "utils/jsonb.h"
#endif
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "nodes/memnodes.h"
#include "utils/memutils.h"
#include "fmgr.h"
} // extern "C"


using namespace v8;

static Datum ToScalarDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type);
static Datum ToArrayDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type);
static Datum ToRecordDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type);
static Local<v8::Value> ToScalarValue(Datum datum, bool isnull, plv8_type *type);
static Local<v8::Value> ToArrayValue(Datum datum, bool isnull, plv8_type *type);
static Local<v8::Value> ToRecordValue(Datum datum, bool isnull, plv8_type *type);
static double TimestampTzToEpoch(TimestampTz tm);
static Datum EpochToTimestampTz(double epoch);
static double DateToEpoch(DateADT date);
static Datum EpochToDate(double epoch);

void
plv8_fill_type(plv8_type *type, Oid typid, MemoryContext mcxt)
{
	bool    ispreferred;

	if (!mcxt)
		mcxt = CurrentMemoryContext;

	type->typid = typid;
	type->fn_input.fn_mcxt = type->fn_output.fn_mcxt = mcxt;
	get_type_category_preferred(typid, &type->category, &ispreferred);
	type->is_composite = (type->category == TYPCATEGORY_COMPOSITE);
	get_typlenbyvalalign(typid, &type->len, &type->byval, &type->align);

	if (get_typtype(typid) == TYPTYPE_DOMAIN)
	{
		HeapTuple	tp;
		Form_pg_type typtup;

#if PG_VERSION_NUM < 90100
		tp = SearchSysCache(TYPEOID, ObjectIdGetDatum(typid), 0, 0, 0);
#else
		tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
#endif
		if (HeapTupleIsValid(tp))
		{
			/*
			 * Check if the type is the external array types.
			 */
			typtup = (Form_pg_type) GETSTRUCT(tp);
			if (strcmp(NameStr(typtup->typname),
						"plv8_int2array") == 0)
			{
				type->ext_array = kExternalShortArray;
			}
			else if (strcmp(NameStr(typtup->typname),
						"plv8_int4array") == 0)
			{
				type->ext_array = kExternalIntArray;
			}
			else if (strcmp(NameStr(typtup->typname),
						"plv8_float4array") == 0)
			{
				type->ext_array = kExternalFloatArray;
			}
			else if (strcmp(NameStr(typtup->typname),
						"plv8_float8array") == 0)
			{
				type->ext_array = kExternalDoubleArray;
			}
			else if (strcmp(NameStr(typtup->typname),
						"plv8_int8array") == 0)
			{
				type->ext_array = kExternalInt64Array;
			}

			ReleaseSysCache(tp);
		}
		else
			elog(ERROR, "cache lookup failed for type %d", typid);

		if (type->ext_array)
			return;

		/* If not, do as usual. */
	}

	if (type->category == TYPCATEGORY_ARRAY)
	{
		Oid      elemid = get_element_type(typid);

		if (elemid == InvalidOid)
			ereport(ERROR,
				(errmsg("cannot determine element type of array: %u", typid)));

		type->typid = elemid;
		type->is_composite = (TypeCategory(elemid) == TYPCATEGORY_COMPOSITE);
		get_typlenbyvalalign(type->typid, &type->len, &type->byval, &type->align);
	}
}

/*
 * Return the database type inferred by the JS value type.
 * If none looks appropriate, InvalidOid is returned (currently,
 * objects and arrays are in this case).
 */
Oid
inferred_datum_type(Handle<v8::Value> value)
{
	if (value->IsUndefined() || value->IsNull())
		return TEXTOID;
	if (value->IsBoolean())
		return BOOLOID;
	else if (value->IsInt32())
		return INT4OID;
	else if (value->IsUint32())
		return INT8OID;
	else if (value->IsBigInt())
		return INT8OID;
	else if (value->IsNumber())
		return FLOAT8OID;
	else if (value->IsString())
		return TEXTOID;
	else if (value->IsDate())
		return TIMESTAMPOID;
/*
	else if (value->IsObject())
	else if (value->IsArray())
*/

	return InvalidOid;
}

#if PG_VERSION_NUM >= 90400 && JSONB_DIRECT_CONVERSION

// jsonb types moved in pg10
#if PG_VERSION_NUM < 100000
#define jbvString JsonbValue::jbvString
#define jbvNumeric JsonbValue::jbvNumeric
#define jbvBool JsonbValue::jbvBool
#define jbvObject JsonbValue::jbvObject
#define jbvArray JsonbValue::jbvArray
#define jbvNull JsonbValue::jbvNull
#endif

static Local<v8::Value>
GetJsonbValue(JsonbValue *scalarVal) {
  Isolate *isolate = Isolate::GetCurrent();

  if (scalarVal->type == jbvNull) {
		return Local<v8::Value>::New(isolate, Null(isolate));
  } else if (scalarVal->type == jbvString) {
    char t[ scalarVal->val.string.len + 1 ];
    strncpy(t, scalarVal->val.string.val, scalarVal->val.string.len);
    t[ scalarVal->val.string.len ] = '\0';

		return Local<v8::Value>::New(isolate, String::NewFromUtf8(isolate, t));
  } else if (scalarVal->type == jbvNumeric) {
		return Local<v8::Value>::New(isolate, Number::New(isolate, DatumGetFloat8(DirectFunctionCall1(numeric_float8, PointerGetDatum(scalarVal->val.numeric)))));
  } else if (scalarVal->type == jbvBool) {
		return Local<v8::Value>::New(isolate, Boolean::New(isolate, scalarVal->val.boolean));
  } else {
    elog(ERROR, "unknown jsonb scalar type");
    return Local<v8::Value>::New(isolate, Null(isolate));
  }
}

static Local<v8::Object>
JsonbIterate(JsonbIterator **it, Local<v8::Object> container) {
  Isolate *isolate = Isolate::GetCurrent();
  JsonbValue val;
	Local<v8::Value> out;
	int32 count = 0;
  JsonbIteratorToken token;
	Local<v8::Value> key;
	Local<v8::Object> obj;

  token = JsonbIteratorNext(it, &val, false);
  while (token != WJB_DONE) {
    switch (token) {
    case WJB_BEGIN_OBJECT:
			obj = v8::Object::New(isolate);
			if (container->IsArray()) {
				container->Set(count, JsonbIterate(it, obj));
				count++;
			} else {
				container->Set(key, JsonbIterate(it, obj));
			}
      break;

    case WJB_END_OBJECT:
      return container;

      break;

    case WJB_BEGIN_ARRAY:
			obj = v8::Array::New(isolate);
			if (container->IsArray()) {
				container->Set(count, JsonbIterate(it, obj));
				count++;
			} else {
				container->Set(key, JsonbIterate(it, obj));
			}
      break;

    case WJB_END_ARRAY:
      return container;

      break;

    case WJB_KEY:
			key = GetJsonbValue(&val);

      break;

    case WJB_VALUE:
      // object value
			container->Set(key, GetJsonbValue(&val));
      break;

    case WJB_ELEM:
      // array element
			container->Set(count, GetJsonbValue(&val));
			count++;
      break;

    case WJB_DONE:
      return container;
      break;

    default:
      elog(ERROR, "unknown jsonb iterator value");
    }

    token = JsonbIteratorNext(it, &val, false);
  }

  return container;
}

static Local<Object>
ConvertJsonb(JsonbContainer *in) {
	Isolate *isolate = Isolate::GetCurrent();
	JsonbValue val;
	JsonbIterator *it = JsonbIteratorInit(in);
	JsonbIteratorToken token = JsonbIteratorNext(&it, &val, false);

	Local<Object> container;

	if (token == WJB_BEGIN_ARRAY) {
		container = v8::Array::New(isolate);
	} else {
		container = v8::Object::New(isolate);
	}

	return JsonbIterate(&it, container);
}

static JsonbValue *
JsonbObjectFromObject(JsonbParseState **pstate, Local<v8::Object> object);
static JsonbValue *
JsonbArrayFromArray(JsonbParseState **pstate, Local<v8::Object> object);

static void LogType(Local<v8::Value> val, bool asError = true) {
	if( val->IsUndefined() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Undefined");
	if( val->IsNull() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Null");
	if( val->IsTrue() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: True");
	if( val->IsFalse() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: False");
	if( val->IsName() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Name");
	if( val->IsString() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: String");
	if( val->IsSymbol() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Symbol");
	if( val->IsFunction() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Function");
	if( val->IsArray() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Array");
	if( val->IsObject() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Object");
	if( val->IsBoolean() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Boolean");
	if( val->IsNumber() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Number");
	if( val->IsExternal() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: External");
	if( val->IsInt32() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Int32");
	if( val->IsUint32() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Uint32");
	if( val->IsDate() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Date");
	if( val->IsArgumentsObject() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Arguments Object");
	if( val->IsBooleanObject() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Boolean Object");
	if( val->IsNumberObject() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Number Object");
	if( val->IsStringObject() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: String Object");
	if( val->IsSymbolObject() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Symbol Object");
	if( val->IsNativeError() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Native Error");
	if( val->IsRegExp() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: RegExp");
	if( val->IsGeneratorFunction() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Generator Function");
	if( val->IsGeneratorObject() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Generator Object");
	if( val->IsPromise() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Promise");
	if( val->IsMap() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Map");
	if( val->IsSet() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Set");
	if( val->IsMapIterator() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Map Iterator");
	if( val->IsSetIterator() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Set Iterator");
	if( val->IsWeakMap() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Weak Map");
	if( val->IsWeakSet() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Weak Set");
	if( val->IsArrayBuffer() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Array Buffer");
	if( val->IsArrayBufferView() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Array Buffer View");
	if( val->IsTypedArray() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Typed Array");
	if( val->IsUint8Array() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Uint8 Array");
	if( val->IsUint8ClampedArray() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Uint8 Clamped Array");
	if( val->IsInt8Array() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Int8 Array");
	if( val->IsUint16Array() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Uint16 Array");
	if( val->IsInt16Array() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Int16 Array");
	if( val->IsUint32Array() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Uint32 Array");
	if( val->IsInt32Array() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Int32 Array");
	if( val->IsFloat32Array() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Float32 Array");
	if( val->IsFloat64Array() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Float64 Array");
	if( val->IsDataView() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Data View");
	if( val->IsSharedArrayBuffer() )
	  elog((asError ? ERROR : NOTICE), "Unaccounted for type: Shared Buffer Array");
}

static char *
TimeAs8601 (double millis) {
	char tmp[100];
	char *buf = (char *)palloc(25);

	time_t t = (time_t) (millis / 1000);
	strftime (tmp, 25, "%Y-%m-%dT%H:%M:%S", gmtime(&t));

	double integral;
	double fractional = modf(millis / 1000, &integral);

	sprintf(buf, "%s.%03dZ", tmp, (int) (fractional * 1000));

	return buf;
}

static JsonbValue *
JsonbFromValue(JsonbParseState **pstate, Local<v8::Value> value, JsonbIteratorToken type) {
	Isolate *isolate = Isolate::GetCurrent();
	JsonbValue val;

	// if the token type is a key, the only valid value is jbvString
	if (type == WJB_KEY) {
		val.type = jbvString;
		String::Utf8Value utf8(isolate, value->ToString(isolate));
		val.val.string.val = ToCStringCopy(utf8);
		val.val.string.len = utf8.length();
	} else {
		if (value->IsBoolean()) {
			val.type = jbvBool;
			val.val.boolean = value->BooleanValue(isolate->GetCurrentContext()).ToChecked();
		} else if (value->IsNull()) {
			val.type = jbvNull;
		} else if (value->IsUndefined()) {
			return NULL;
		} else if (value->IsString()) {
			val.type = jbvString;
			String::Utf8Value utf8(isolate, value->ToString(isolate));
			val.val.string.val = ToCStringCopy(utf8);
			val.val.string.len = utf8.length();
		} else if (value->IsNumber()) {
			if (value->IsInt32()) {
				int32 iv = value->Int32Value(isolate->GetCurrentContext()).ToChecked();
				val.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(iv)));
				val.type = jbvNumeric;
			} else if (value->IsUint32()) {
				int64 iv = (int64) value->Uint32Value(isolate->GetCurrentContext()).ToChecked();
				val.val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric, Int64GetDatum(iv)));
				val.type = jbvNumeric;
			} else {
				float8 fv = (float8) value->NumberValue(isolate->GetCurrentContext()).ToChecked();

				val.val.numeric = DatumGetNumeric(DirectFunctionCall1(float8_numeric, Float8GetDatum(fv)));
				val.type = jbvNumeric;
			}
		} else if (value->IsDate()) {
			double t = value->NumberValue(isolate->GetCurrentContext()).ToChecked();
			if (isnan(t)) {
				val.type = jbvNull;
			} else {
				val.val.string.val = TimeAs8601(t);
				val.val.string.len = 24;
				val.type = jbvString;
			}
		} else {
			LogType(value, false);
			val.type = jbvString;
			String::Utf8Value utf8(isolate, value->ToString(isolate));
			val.val.string.val = ToCStringCopy(utf8);
			val.val.string.len = utf8.length();
		}
	}

	return pushJsonbValue(pstate, type, &val);
}

static JsonbValue *
JsonbArrayFromArray(JsonbParseState **pstate, Local<v8::Object> object) {
	JsonbValue *val = pushJsonbValue(pstate, WJB_BEGIN_ARRAY, NULL);
	Local<v8::Array> a = Local<v8::Array>::Cast(object);
	for (size_t i = 0; i < a->Length(); i++) {
		Local<v8::Value> o = a->Get(i);

		if (o->IsArray()) {
			val = JsonbArrayFromArray(pstate, Local<v8::Array>::Cast(o));
		} else if (o->IsObject()) {
			val = JsonbObjectFromObject(pstate, Local<v8::Object>::Cast(o));
		} else {
			val = JsonbFromValue(pstate, o, WJB_ELEM);
		}
	}

	val = pushJsonbValue(pstate, WJB_END_ARRAY, NULL);

	return val;
}

static JsonbValue *
JsonbObjectFromObject(JsonbParseState **pstate, Local<v8::Object> object) {
	Isolate *isolate = Isolate::GetCurrent();
	JsonbValue *val = pushJsonbValue(pstate, WJB_BEGIN_OBJECT, NULL);
	Local<Array> arr = object->GetOwnPropertyNames(isolate->GetCurrentContext()).ToLocalChecked();

	for (size_t i = 0; i < arr->Length(); i++) {
		Local<v8::Value> v = arr->Get(i);
		val = JsonbFromValue(pstate, v, WJB_KEY);
		Local<v8::Value> o = object->Get(v);

		if (o->IsDate()) {
			val = JsonbFromValue(pstate, o, WJB_VALUE);
		} else if (o->IsArray()) {
			val = JsonbArrayFromArray(pstate, Local<v8::Array>::Cast(o));
		} else if (o->IsObject()) {
			val = JsonbObjectFromObject(pstate, Local<v8::Object>::Cast(o));
		} else {
			val = JsonbFromValue(pstate, o, WJB_VALUE);
		}
	}
	val = pushJsonbValue(pstate, WJB_END_OBJECT, NULL);
	return val;
}

static Jsonb *
ConvertObject(Local<v8::Object> object) {
	// create a new memory context for conversion
	MemoryContext oldcontext = CurrentMemoryContext;
	MemoryContext conversion_context;

#if PG_VERSION_NUM < 110000
	conversion_context = AllocSetContextCreate(
						CurrentMemoryContext,
						"JSONB Conversion Context",
						ALLOCSET_SMALL_MINSIZE,
						ALLOCSET_SMALL_INITSIZE,
						ALLOCSET_SMALL_MAXSIZE);
#else
	conversion_context = AllocSetContextCreate(CurrentMemoryContext,
						"JSONB Conversion Context",
						ALLOCSET_SMALL_SIZES);
#endif

	MemoryContextSwitchTo(conversion_context);

  JsonbParseState *pstate = NULL;
  JsonbValue *val;

	if (object->IsArray()) {
		val = JsonbArrayFromArray(&pstate, object);
	} else if (object->IsObject()) {
		val = JsonbObjectFromObject(&pstate, object);
	} else {
		val = pushJsonbValue(&pstate, WJB_BEGIN_ARRAY, NULL);
		val = JsonbFromValue(&pstate, object, WJB_ELEM);
		val = pushJsonbValue(&pstate, WJB_END_ARRAY, NULL);
	}

	MemoryContextSwitchTo(oldcontext);

	Jsonb *ret = JsonbValueToJsonb(val);
	MemoryContextDelete(conversion_context);
  return ret;
}
#endif

static Local<Object>
CreateExternalArray(void *data, plv8_external_array_type array_type,
					int byte_size, Datum datum)
{
	Isolate* isolate = Isolate::GetCurrent();
	Local<v8::ArrayBuffer> buffer;
	Local<v8::TypedArray> array;

	buffer = v8::ArrayBuffer::New(isolate, byte_size);
	if (buffer.IsEmpty())
	{
		return {};
	}

	switch (array_type)
	{
	case kExternalByteArray:
		array = v8::Int8Array::New(buffer, 0, byte_size);
		break;
	case kExternalUnsignedByteArray:
		array = v8::Uint8Array::New(buffer, 0, byte_size);
		break;
	case kExternalShortArray:
		array = v8::Int16Array::New(buffer, 0, byte_size / sizeof(int16));
		break;
	case kExternalUnsignedShortArray:
		array = v8::Uint16Array::New(buffer, 0, byte_size / sizeof(int16));
		break;
	case kExternalIntArray:
		array = v8::Int32Array::New(buffer, 0, byte_size / sizeof(int32));
		break;
	case kExternalUnsignedIntArray:
		array = v8::Uint32Array::New(buffer, 0, byte_size / sizeof(int32));
		break;
	case kExternalFloatArray:
		array = v8::Float32Array::New(buffer, 0, byte_size / sizeof(float4));
		break;
	case kExternalDoubleArray:
		array = v8::Float64Array::New(buffer, 0, byte_size / sizeof(float8));
		break;
	case kExternalInt64Array:
		array = v8::BigInt64Array::New(buffer, 0, byte_size / sizeof(int64));
	default:
		throw js_error("unexpected array type");
	}
	array->SetInternalField(0, External::New(isolate, DatumGetPointer(datum)));

	// needs to be a copy, as the data could go away
	memcpy(buffer->GetContents().Data(), data, byte_size);

	return array;
}

static void *
ExtractExternalArrayDatum(Handle<v8::Value> value)
{
	if (value->IsUndefined() || value->IsNull()) {
		return NULL;
	}

	if (value->IsTypedArray())
	{
		Handle<Object> object = Handle<Object>::Cast(value);
		return Handle<External>::Cast(object->GetInternalField(0))->Value();
	}

	return NULL;
}

Datum
ToDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type)
{
	if (type->category == TYPCATEGORY_ARRAY)
		return ToArrayDatum(value, isnull, type);
	else
		return ToScalarDatum(value, isnull, type);
}

static Datum
ToScalarDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type)
{
	Isolate* isolate = Isolate::GetCurrent();
	if (type->category == TYPCATEGORY_COMPOSITE)
		return ToRecordDatum(value, isnull, type);

	if (value->IsUndefined() || value->IsNull())
	{
		*isnull = true;
		return (Datum) 0;
	}

	*isnull = false;
	switch (type->typid)
	{
	case OIDOID:
		if (value->IsNumber())
			return ObjectIdGetDatum(value->Uint32Value(isolate->GetCurrentContext()).ToChecked());
		break;
	case BOOLOID:
		if (value->IsBoolean())
			return BoolGetDatum(value->BooleanValue(isolate->GetCurrentContext()).ToChecked());
		break;
	case INT2OID:
		if (value->IsNumber())
#ifdef CHECK_INTEGER_OVERFLOW
			return DirectFunctionCall1(int82,
					Int64GetDatum(value->IntegerValue()));
#else
			return Int16GetDatum((int16) value->Int32Value(isolate->GetCurrentContext()).ToChecked());
#endif
		break;
	case INT4OID:
		if (value->IsNumber())
#ifdef CHECK_INTEGER_OVERFLOW
			return DirectFunctionCall1(int84,
					Int64GetDatum(value->IntegerValue()));
#else
			return Int32GetDatum((int32) value->Int32Value(isolate->GetCurrentContext()).ToChecked());
#endif
		break;
	case INT8OID:
		if (value->IsBigInt()) {
			BigInt *b = BigInt::Cast(*value);
			return Int64GetDatum((int64) b->Int64Value());
		}
		if (value->IsNumber())
			return Int64GetDatum((int64) value->IntegerValue(isolate->GetCurrentContext()).ToChecked());
		break;
	case FLOAT4OID:
		if (value->IsNumber())
			return Float4GetDatum((float4) value->NumberValue(isolate->GetCurrentContext()).ToChecked());
		break;
	case FLOAT8OID:
		if (value->IsNumber())
			return Float8GetDatum((float8) value->NumberValue(isolate->GetCurrentContext()).ToChecked());
		break;
	case NUMERICOID:
		if (value->IsBigInt()) {
			String::Utf8Value utf8(isolate, value->ToString(isolate));
			return DirectFunctionCall3(numeric_in, (Datum) *utf8, ObjectIdGetDatum(InvalidOid), Int32GetDatum((int32) -1));
		}
		if (value->IsNumber())
			return DirectFunctionCall1(float8_numeric,
					Float8GetDatum((float8) value->NumberValue(isolate->GetCurrentContext()).ToChecked()));
		break;
	case DATEOID:
		if (value->IsDate())
			return EpochToDate(value->NumberValue(isolate->GetCurrentContext()).ToChecked());
		break;
	case TIMESTAMPOID:
	case TIMESTAMPTZOID:
		if (value->IsDate())
			return EpochToTimestampTz(value->NumberValue(isolate->GetCurrentContext()).ToChecked());
		break;
	case BYTEAOID:
		{
			if (value->IsUint8Array() || value->IsInt8Array()) {
				v8::Handle<v8::Uint8Array> array = v8::Handle<v8::Uint8Array>::Cast(value);
				void *data = array->Buffer()->GetContents().Data();
				int		len = array->Length();
				size_t		size = len + VARHDRSZ;
				void	   *result = (void *) palloc(size);

				SET_VARSIZE(result, size);
				memcpy(VARDATA(result), data, len);
				return PointerGetDatum(result);
			}

			if (value->IsUint16Array() || value->IsInt16Array()) {
				v8::Handle<v8::Uint16Array> array = v8::Handle<v8::Uint16Array>::Cast(value);
				void *data = array->Buffer()->GetContents().Data();
				int		len = array->Length();
				size_t		size = (len * 2) + VARHDRSZ;
				void	   *result = (void *) palloc(size);

				SET_VARSIZE(result, size);
				memcpy(VARDATA(result), data, len * 2);
				return PointerGetDatum(result);
			}

			if (value->IsUint32Array() || value->IsInt32Array()) {
				v8::Handle<v8::Uint32Array> array = v8::Handle<v8::Uint32Array>::Cast(value);
				void *data = array->Buffer()->GetContents().Data();
				int		len = array->Length();
				size_t		size = (len * 4) + VARHDRSZ;
				void	   *result = (void *) palloc(size);

				SET_VARSIZE(result, size);
				memcpy(VARDATA(result), data, len * 4);
				return PointerGetDatum(result);
			}

			if (value->IsArrayBuffer()) {
				v8::Handle<v8::ArrayBuffer> array = v8::Handle<v8::ArrayBuffer>::Cast(value);
				void *data = array->GetContents().Data();
				int		len = array->ByteLength();
				size_t		size = len + VARHDRSZ;
				void	   *result = (void *) palloc(size);

				SET_VARSIZE(result, size);
				memcpy(VARDATA(result), data, len);
				return PointerGetDatum(result);
			}

			void *datum_p = ExtractExternalArrayDatum(value);

			if (datum_p)
			{
				return PointerGetDatum(datum_p);
			}
		}
#if PG_VERSION_NUM >= 90400
	case JSONBOID:
#if JSONB_DIRECT_CONVERSION
		{
			Jsonb *obj = ConvertObject(Local<v8::Object>::Cast(value));
#if PG_VERSION_NUM < 110000
			PG_RETURN_JSONB(DatumGetJsonb(obj));
#else
			PG_RETURN_JSONB_P(DatumGetJsonbP(obj));
#endif // PG_VERSION_NUM < 110000
		}
#else // JSONB_DIRECT_CONVERSION
		if (value->IsObject() || value->IsArray())
		{
			JSONObject JSON;

			Handle<v8::Value> result = JSON.Stringify(value);
			CString str(result);

#if PG_VERSION_NUM < 110000
			// lots of casting, but it ends up working - there is no CStringGetJsonb exposed
			return (Datum) DatumGetJsonb(DirectFunctionCall1(jsonb_in, (Datum) (char *) str));
#else
			return (Datum) DatumGetJsonbP(DirectFunctionCall1(jsonb_in, (Datum) (char *) str));
#endif
		}
#endif // JSONB_DIRECT_CONVERSION
		break;
#endif
#if PG_VERSION_NUM >= 90200
	case JSONOID:
		if (value->IsObject() || value->IsArray())
		{
			JSONObject JSON;

			Handle<v8::Value> result = JSON.Stringify(value);
			CString str(result);

			return CStringGetTextDatum(str);
		}
		break;
#endif
	}

	/* Use lexical cast for non-numeric types. */
	CString		str(value);
	Datum		result;

	PG_TRY();
	{
		if (type->fn_input.fn_addr == NULL)
		{
			Oid    input_func;

			getTypeInputInfo(type->typid, &input_func, &type->ioparam);
			fmgr_info_cxt(input_func, &type->fn_input, type->fn_input.fn_mcxt);
		}
		result = InputFunctionCall(&type->fn_input, str, type->ioparam, -1);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return result;
}

static Datum
ToArrayDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type)
{
	int			length;
	Datum	   *values;
	bool	   *nulls;
	int			ndims[1];
	int			lbs[] = {1};
	ArrayType  *result;

	if (value->IsUndefined() || value->IsNull())
	{
		*isnull = true;
		return (Datum) 0;
	}

	void *datum_p = ExtractExternalArrayDatum(value);
	if (datum_p)
	{
		*isnull = false;
		return PointerGetDatum(datum_p);
	}

	Handle<Array> array(Handle<Array>::Cast(value));
	if (array.IsEmpty() || !array->IsArray())
		throw js_error("value is not an Array");

	length = array->Length();
	values = (Datum *) palloc(sizeof(Datum) * length);
	nulls = (bool *) palloc(sizeof(bool) * length);
	ndims[0] = length;
	for (int i = 0; i < length; i++) {
		if (type->is_composite)
		{
			values[i] = ToRecordDatum(array->Get(i), &nulls[i], type);
		} else {
			values[i] = ToScalarDatum(array->Get(i), &nulls[i], type);
		}
	}

	result = construct_md_array(values, nulls, 1, ndims, lbs,
				type->typid, type->len, type->byval, type->align);
	pfree(values);
	pfree(nulls);

	*isnull = false;
	return PointerGetDatum(result);
}

static Datum
ToRecordDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type)
{
	Datum		result;
	TupleDesc	tupdesc;

	if (value->IsUndefined() || value->IsNull())
	{
		*isnull = true;
		return (Datum) 0;
	}

	PG_TRY();
	{
		tupdesc = lookup_rowtype_tupdesc(type->typid, -1);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	Converter	conv(tupdesc);

	result = conv.ToDatum(value);

	ReleaseTupleDesc(tupdesc);

	*isnull = false;
	return result;
}

Local<v8::Value>
ToValue(Datum datum, bool isnull, plv8_type *type)
{
	Isolate* isolate = Isolate::GetCurrent();
	if (isnull)
		return Local<v8::Value>::New(isolate, Null(isolate));
	else if (type->category == TYPCATEGORY_ARRAY || type->typid == RECORDARRAYOID)
		return ToArrayValue(datum, isnull, type);
	else if (type->category == TYPCATEGORY_COMPOSITE || type->typid == RECORDOID)
		return ToRecordValue(datum, isnull, type);
	else
		return ToScalarValue(datum, isnull, type);
}

static Local<v8::Value>
ToScalarValue(Datum datum, bool isnull, plv8_type *type)
{
	Isolate* isolate = Isolate::GetCurrent();
	switch (type->typid)
	{
	case OIDOID:
		return Uint32::New(isolate, DatumGetObjectId(datum));
	case BOOLOID:
		return Boolean::New(isolate, DatumGetBool(datum));
	case INT2OID:
		return Int32::New(isolate, DatumGetInt16(datum));
	case INT4OID:
		return Int32::New(isolate, DatumGetInt32(datum));
	case INT8OID: {
#if BIGINT_GRACEFUL
		int64 v = DatumGetInt64(datum);

		if (v > INT32_MAX || v < INT32_MIN)
		{
			char str[20];
			sprintf(str, "%ld", v);
			return ToString(str);
		}
		return Number::New(isolate, v);
#else
		return BigInt::New(isolate, DatumGetInt64(datum));
#endif
	}
	case FLOAT4OID:
		return Number::New(isolate, DatumGetFloat4(datum));
	case FLOAT8OID:
		return Number::New(isolate, DatumGetFloat8(datum));
	case NUMERICOID:
		return Number::New(isolate, DatumGetFloat8(
			DirectFunctionCall1(numeric_float8, datum)));
	case DATEOID:
		return Date::New(isolate->GetCurrentContext(), DateToEpoch(DatumGetDateADT(datum))).ToLocalChecked();
	case TIMESTAMPOID:
	case TIMESTAMPTZOID:
		return Date::New(isolate->GetCurrentContext(), TimestampTzToEpoch(DatumGetTimestampTz(datum))).ToLocalChecked();
	case TEXTOID:
	case VARCHAROID:
	case BPCHAROID:
	case XMLOID:
	{
		void	   *p = PG_DETOAST_DATUM_PACKED(datum);
		const char *str = VARDATA_ANY(p);
		int			len = VARSIZE_ANY_EXHDR(p);

		Local<String>	result = ToString(str, len);

		if (p != DatumGetPointer(datum))
			pfree(p);	// free if detoasted
		return result;
	}
	case BYTEAOID:
	{
		void	   *p = PG_DETOAST_DATUM_COPY(datum);

		return CreateExternalArray(VARDATA_ANY(p),
								   kExternalUnsignedByteArray,
								   VARSIZE_ANY_EXHDR(p),
								   PointerGetDatum(p));
	}
#if PG_VERSION_NUM >= 90200
	case JSONOID:
	{
		void	   *p = PG_DETOAST_DATUM_PACKED(datum);
		const char *str = VARDATA_ANY(p);
		int			len = VARSIZE_ANY_EXHDR(p);

		Local<v8::Value>	jsonString = ToString(str, len);
		JSONObject JSON;
		Local<v8::Value> result = Local<v8::Value>::New(isolate, JSON.Parse(jsonString));

		if (p != DatumGetPointer(datum))
			pfree(p);	// free if detoasted
		return result;
	}
#endif
#if PG_VERSION_NUM >= 90400
	case JSONBOID:
	{
#if JSONB_DIRECT_CONVERSION
		Jsonb *jsonb = (Jsonb *) PG_DETOAST_DATUM(datum);
		Local<v8::Value> result = ConvertJsonb(&jsonb->root);
#else
		Local<v8::Value>	jsonString = ToString(datum, type);
		JSONObject JSON;
		Local<v8::Value> result = Local<v8::Value>::New(isolate, JSON.Parse(jsonString));
#endif

		return result;
	}
#endif
	default:
		return ToString(datum, type);
	}
}

static Local<v8::Value>
ToArrayValue(Datum datum, bool isnull, plv8_type *type)
{
	Datum	   *values;
	bool	   *nulls;
	int			nelems;

	/*
	 * If we can use an external array, do it instead.
	 */
	if (type->ext_array)
	{
		ArrayType   *array = DatumGetArrayTypePCopy(datum);

		/*
		 * We allow only non-NULL, 1-dim array.
		 */
		if (!ARR_HASNULL(array) && ARR_NDIM(array) <= 1)
		{
			int			data_bytes = ARR_SIZE(array) -
										ARR_OVERHEAD_NONULLS(1);
			return CreateExternalArray(ARR_DATA_PTR(array),
									   type->ext_array,
									   data_bytes,
									   PointerGetDatum(array));
		}

		throw js_error("NULL element, or multi-dimension array not allowed"
						" in external array type");
	}

	deconstruct_array(DatumGetArrayTypeP(datum),
						type->typid, type->len, type->byval, type->align,
						&values, &nulls, &nelems);
	Local<Array>  result = Array::New(Isolate::GetCurrent(), nelems);
	plv8_type base = { 0 };
	bool    ispreferred;

	base.typid = type->typid;
	if (base.typid == RECORDARRAYOID)
		base.typid = RECORDOID;

	base.fn_input.fn_mcxt = base.fn_output.fn_mcxt = type->fn_input.fn_mcxt;
	get_type_category_preferred(base.typid, &(base.category), &ispreferred);
	get_typlenbyvalalign(base.typid, &(base.len), &(base.byval), &(base.align));

	for (int i = 0; i < nelems; i++)
		result->Set(i, ToValue(values[i], nulls[i], &base));

	pfree(values);
	pfree(nulls);

	return result;
}

static Local<v8::Value>
ToRecordValue(Datum datum, bool isnull, plv8_type *type)
{
	HeapTupleHeader	rec = DatumGetHeapTupleHeader(datum);
	Oid				tupType;
	int32			tupTypmod;
	TupleDesc		tupdesc;
	HeapTupleData	tuple;

	PG_TRY();
	{
		/* Extract type info from the tuple itself */
		tupType = HeapTupleHeaderGetTypeId(rec);
		tupTypmod = HeapTupleHeaderGetTypMod(rec);
		tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	Converter	conv(tupdesc);

	/* Build a temporary HeapTuple control structure */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = rec;

	Local<v8::Value> result = conv.ToValue(&tuple);

	ReleaseTupleDesc(tupdesc);

	return result;
}

Local<String>
ToString(Datum value, plv8_type *type)
{
	int		encoding = GetDatabaseEncoding();
	char   *str;

	PG_TRY();
	{
		if (type->fn_output.fn_addr == NULL)
		{
			Oid		output_func;
			bool	isvarlen;

			getTypeOutputInfo(type->typid, &output_func, &isvarlen);
			fmgr_info_cxt(output_func, &type->fn_output, type->fn_output.fn_mcxt);
		}
		str = OutputFunctionCall(&type->fn_output, value);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	Local<String>	result =
		encoding == PG_UTF8
			? String::NewFromUtf8(Isolate::GetCurrent(), str)
			: ToString(str, strlen(str), encoding);
	pfree(str);

	return result;
}

Local<String>
ToString(const char *str, int len, int encoding)
{
	char		   *utf8;
	Isolate		   *isolate = Isolate::GetCurrent();

	if (str == NULL) {
		return String::NewFromUtf8(isolate, "(null)", String::kNormalString, 6);
	}
	if (len < 0)
		len = strlen(str);

	PG_TRY();
	{
		utf8 = (char *) pg_do_encoding_conversion(
					(unsigned char *) str, len, encoding, PG_UTF8);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	if (utf8 != str)
		len = strlen(utf8);
	Local<String> result = String::NewFromUtf8(isolate, utf8, String::kNormalString, len);
	if (utf8 != str)
		pfree(utf8);
	return result;
}

/*
 * Convert utf8 text to database encoded text.
 * The result could be same as utf8 input, or palloc'ed one.
 */
char *
ToCString(const String::Utf8Value &value)
{
	char *str = const_cast<char *>(*value);
	if (str == NULL)
		return NULL;

	int    encoding = GetDatabaseEncoding();
	if (encoding == PG_UTF8)
		return str;

	PG_TRY();
	{
		str = (char *) pg_do_encoding_conversion(
				(unsigned char *) str, strlen(str), PG_UTF8, encoding);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return str;
}

/*
 * Convert utf8 text to database encoded text.
 * The result is always palloc'ed one.
 */
char *
ToCStringCopy(const String::Utf8Value &value)
{
	char *str;
	const char *utf8 = *value;
	if (utf8 == NULL)
		return NULL;

	PG_TRY();
	{
		int	encoding = GetDatabaseEncoding();
		str = (char *) pg_do_encoding_conversion(
				(unsigned char *) utf8, strlen(utf8), PG_UTF8, encoding);
		if (str == utf8)
			str = pstrdup(utf8);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return str;
}

/*
 * Since v8 represents a Date object using a double value in msec from unix epoch,
 * we need to shift the epoch and adjust the time unit.
 */
static double
TimestampTzToEpoch(TimestampTz tm)
{
	double		epoch;

	// TODO: check if TIMESTAMP_NOBEGIN or NOEND
#ifdef HAVE_INT64_TIMESTAMP
	epoch = (double) tm / 1000.0;
#else
	epoch = (double) tm * 1000.0;
#endif

	return epoch + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;
}

static Datum
EpochToTimestampTz(double epoch)
{
	epoch -= (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;

#ifdef HAVE_INT64_TIMESTAMP
	return Int64GetDatum((int64) epoch * 1000);
#else
	return Float8GetDatum(epoch / 1000.0);
#endif
}

static double
DateToEpoch(DateADT date)
{
	double		epoch;

	// TODO: check if DATE_NOBEGIN or NOEND
#ifdef HAVE_INT64_TIMESTAMP
	epoch = (double) date * USECS_PER_DAY / 1000.0;
#else
	epoch = (double) date * SECS_PER_DAY * 1000.0;
#endif

	return epoch + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;
}

static Datum
EpochToDate(double epoch)
{
	epoch -= (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;

#ifdef HAVE_INT64_TIMESTAMP
	epoch = (epoch * 1000) / USECS_PER_DAY;
#else
    epoch = (epoch / 1000) / SECS_PER_DAY;
#endif
	PG_RETURN_DATEADT((DateADT) epoch);
}

CString::CString(Handle<v8::Value> value) : m_utf8(Isolate::GetCurrent(), value)
{
	m_str = ToCString(m_utf8);
}

CString::~CString()
{
	if (m_str != *m_utf8)
		pfree(m_str);
}

bool CString::toStdString(v8::Handle<v8::Value> value, std::string &out)
{
	if(value.IsEmpty()) return false;
	Isolate* isolate = Isolate::GetCurrent();
	String::Utf8Value utf8(isolate, value->ToString(isolate));

  // convert it to string
	//auto obj = value->ToString(isolate);
	//String::Utf8Value utf8(val);
	if(*utf8) {
		out = *utf8;
		return true;
	}
	return false;
}
