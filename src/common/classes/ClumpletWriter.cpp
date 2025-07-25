/*
 *	PROGRAM:	Client/Server Common Code
 *	MODULE:		ClumpletWriter.cpp
 *	DESCRIPTION:	Secure handling of clumplet buffers
 *
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Nickolay Samofatov
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2004 Nickolay Samofatov <nickolay@broadviewsoftware.com>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 *
 *
 */

#include "firebird.h"

#include "../common/classes/ClumpletWriter.h"
#include "../common/classes/MetaString.h"
#include "fb_exception.h"
#include "ibase.h"

namespace Firebird {

ClumpletWriter::ClumpletWriter(Kind k, FB_SIZE_T limit, UCHAR tag)
	: ClumpletReader(k, NULL, 0),
	  sizeLimit(limit),
	  kindList(NULL),
	  dynamic_buffer(getPool()),
	  flag_overflow(false)
{
	initNewBuffer(tag);
	rewind();
}

ClumpletWriter::ClumpletWriter(MemoryPool& given_pool, Kind k, FB_SIZE_T limit, UCHAR tag)
	: ClumpletReader(given_pool, k, NULL, 0),
	  sizeLimit(limit),
	  kindList(NULL),
	  dynamic_buffer(getPool()),
	  flag_overflow(false)
{
	initNewBuffer(tag);
	rewind();
}

const UCHAR* ClumpletWriter::getBuffer() const
{
	return dynamic_buffer.begin();
}

const UCHAR* ClumpletWriter::getBufferEnd() const
{
	return dynamic_buffer.end();
}

void ClumpletWriter::initNewBuffer(UCHAR tag)
{
	switch (kind)
	{
		case SpbAttach:
			if (tag == isc_spb_version)
			{
				dynamic_buffer.push(isc_spb_version);
			}
			[[fallthrough]];
		case Tagged:
		case Tpb:
		case WideTagged:
			dynamic_buffer.push(tag);
			break;
		default:
			fb_assert(tag == 0);
			break;
	}
}

ClumpletWriter::ClumpletWriter(Kind k, FB_SIZE_T limit, const UCHAR* buffer, FB_SIZE_T buffLen, UCHAR tag)
	: ClumpletReader(k, NULL, 0),
	  sizeLimit(limit),
	  kindList(NULL),
	  dynamic_buffer(getPool()),
	  flag_overflow(false)
{
	create(buffer, buffLen, tag);
}

ClumpletWriter::ClumpletWriter(MemoryPool& pool, const KindList* kl, FB_SIZE_T limit,
							   const UCHAR* buffer, FB_SIZE_T buffLen)
	: ClumpletReader(pool, kl, buffer, buffLen),
	  sizeLimit(limit),
	  kindList(kl),
	  dynamic_buffer(getPool()),
	  flag_overflow(false)
{
	create(buffer, buffLen, kl->tag);
}

ClumpletWriter::ClumpletWriter(const KindList* kl, FB_SIZE_T limit, const UCHAR* buffer, FB_SIZE_T buffLen)
	: ClumpletReader(kl, buffer, buffLen),
	  sizeLimit(limit),
	  kindList(kl),
	  dynamic_buffer(getPool()),
	  flag_overflow(false)
{
	create(buffer, buffLen, kl->tag);
}

ClumpletWriter::ClumpletWriter(MemoryPool& pool, const KindList* kl, FB_SIZE_T limit)
	: ClumpletReader(pool, kl, NULL, 0),
	  sizeLimit(limit),
	  kindList(kl),
	  dynamic_buffer(getPool()),
	  flag_overflow(false)
{
	create(NULL, 0, kl->tag);
}

ClumpletWriter::ClumpletWriter(const KindList* kl, FB_SIZE_T limit)
	: ClumpletReader(kl, NULL, 0),
	  sizeLimit(limit),
	  kindList(kl),
	  dynamic_buffer(getPool()),
	  flag_overflow(false)
{
	create(NULL, 0, kl->tag);
}

ClumpletWriter::ClumpletWriter(MemoryPool& pool, const ClumpletWriter& from)
	: ClumpletReader(pool, from),
	  sizeLimit(from.sizeLimit),
	  kindList(NULL),
	  dynamic_buffer(getPool()),
	  flag_overflow(false)
{
	create(from.getBuffer(), from.getBufferEnd() - from.getBuffer(), from.isTagged() ? from.getBufferTag() : 0);
}

ClumpletWriter::ClumpletWriter(const ClumpletWriter& from)
	: ClumpletReader(from),
	  sizeLimit(from.sizeLimit),
	  kindList(NULL),
	  dynamic_buffer(getPool()),
	  flag_overflow(false)
{
	create(from.getBuffer(), from.getBufferEnd() - from.getBuffer(), from.isTagged() ? from.getBufferTag() : 0);
}

void ClumpletWriter::create(const UCHAR* buffer, FB_SIZE_T buffLen, UCHAR tag)
{
	if (buffer && buffLen) {
		dynamic_buffer.push(buffer, buffLen);
	}
	else {
		initNewBuffer(tag);
	}
	rewind();
}

ClumpletWriter::ClumpletWriter(MemoryPool& given_pool, Kind k, FB_SIZE_T limit,
							   const UCHAR* buffer, FB_SIZE_T buffLen, UCHAR tag)
	: ClumpletReader(given_pool, k, NULL, 0),
	  sizeLimit(limit),
	  kindList(NULL),
	  dynamic_buffer(getPool()),
	  flag_overflow(false)
{
	if (buffer && buffLen) {
		dynamic_buffer.push(buffer, buffLen);
	}
	else {
		initNewBuffer(tag);
	}
	rewind();
}

void ClumpletWriter::reset(UCHAR tag)
{
	if (kindList)
	{
		for (const KindList* kl = kindList; kl->kind != EndOfList; ++kl)
		{
			if (tag == kl->tag)
			{
				kind = kl->kind;
				dynamic_buffer.shrink(0);
				initNewBuffer(tag);
				rewind();

				return;
			}
		}

		invalid_structure("Unknown tag value - missing in the list of possible", tag);
	}

	dynamic_buffer.shrink(0);
	initNewBuffer(tag);
	rewind();
}

void ClumpletWriter::reset(const UCHAR* buffer, const FB_SIZE_T buffLen)
{
	const UCHAR tag = isTagged() ? getBufferTag() : 0;
	dynamic_buffer.clear();
	if (buffer && buffLen) {
		dynamic_buffer.push(buffer, buffLen);
	}
	else
	{
		initNewBuffer(tag);
	}
	rewind();
}

void ClumpletWriter::reset(const ClumpletWriter& from)
{
	reset(from.getBuffer(), from.getBufferEnd() - from.getBuffer());
}

void ClumpletWriter::size_overflow()
{
	fatal_exception::raise("Clumplet buffer size limit reached");
}

void ClumpletWriter::size_overflow(bool condition)
{
	flag_overflow = condition;
	if (condition)
		size_overflow();
}

void ClumpletWriter::toVaxInteger(UCHAR* ptr, FB_SIZE_T length, const SINT64 value)
{
	fb_assert(ptr && length > 0 && length < 9); // We can't handle numbers bigger than int64.
	int shift = 0;
	while (length--)
	{
		*ptr++ = (UCHAR) (value >> shift);
		shift += 8;
	}
}

void ClumpletWriter::insertInt(UCHAR tag, const SLONG value)
{
	UCHAR bytes[sizeof(SLONG)];

	toVaxInteger(bytes, sizeof(bytes), value);
	insertBytesLengthCheck(tag, bytes, sizeof(bytes));
}

void ClumpletWriter::insertBigInt(UCHAR tag, const SINT64 value)
{
	UCHAR bytes[sizeof(SINT64)];

	toVaxInteger(bytes, sizeof(bytes), value);
	insertBytesLengthCheck(tag, bytes, sizeof(bytes));
}

void ClumpletWriter::insertDouble(UCHAR tag, const double value)
{
	union {
		double temp_double;
		SLONG temp_long[2];
	} temp;

	fb_assert(sizeof(double) == sizeof(temp));

	temp.temp_double = value;
	UCHAR bytes[sizeof(double)];
	toVaxInteger(bytes, sizeof(SLONG), temp.temp_long[FB_LONG_DOUBLE_FIRST]);
	toVaxInteger(bytes + sizeof(SLONG), sizeof(SLONG), temp.temp_long[FB_LONG_DOUBLE_SECOND]);
	insertBytesLengthCheck(tag, bytes, sizeof(bytes));
}

void ClumpletWriter::insertTimeStamp(UCHAR tag, const ISC_TIMESTAMP value)
{
	UCHAR bytes[sizeof(ISC_TIMESTAMP)];
	toVaxInteger(bytes, sizeof(SLONG), value.timestamp_date);
	toVaxInteger(bytes + sizeof(SLONG), sizeof(SLONG), value.timestamp_time);
	insertBytesLengthCheck(tag, bytes, sizeof(bytes));
}

void ClumpletWriter::insertString(UCHAR tag, const char* str)
{
	insertString(tag, str, fb_strlen(str));
}

void ClumpletWriter::insertString(UCHAR tag, char* str)
{
	insertString(tag, str, fb_strlen(str));
}

void ClumpletWriter::insertString(UCHAR tag, const char* str, FB_SIZE_T length)
{
	insertBytesLengthCheck(tag, str, length);
}

void ClumpletWriter::insertData(UCHAR tag, const UCharBuffer& data)
{
	insertBytesLengthCheck(tag, data.begin(), data.getCount());
}

void ClumpletWriter::insertBytes(UCHAR tag, const void* bytes, FB_SIZE_T length)
{
	insertBytesLengthCheck(tag, bytes, length);
}

void ClumpletWriter::insertByte(UCHAR tag, const UCHAR byte)
{
	insertBytesLengthCheck(tag, &byte, 1);
}

void ClumpletWriter::insertBytesLengthCheck(UCHAR tag, const void* bytes, const FB_SIZE_T length)
{
	// Check that we're not beyond the end of buffer.
	// We get there when we set end marker.
	if (cur_offset > dynamic_buffer.getCount())
	{
		usage_mistake("write past EOF");
		return;
	}

	UCHAR lenSize = 0;
	// Check length according to clumplet type
	// Perform structure upgrade when needed and possible
	for(;;)
	{
		const ClumpletType t = getClumpletType(tag);
		string m;

		switch (t)
		{
		case Wide:
			if (length > MAX_ULONG)
			{
				m.printf("attempt to store %d bytes in a clumplet", length);
				break;
			}
			lenSize = 4;
			break;
		case TraditionalDpb:
			if (length > MAX_UCHAR)
			{
				m.printf("attempt to store %d bytes in a clumplet with maximum size 255 bytes", length);
				break;
			}
			lenSize = 1;
			break;
		case SingleTpb:
			if (length > 0)
			{
				m.printf("attempt to store data in dataless clumplet");
			}
			break;
		case StringSpb:
			if (length > MAX_USHORT)
			{
				m.printf("attempt to store %d bytes in a clumplet", length);
				break;
			}
			lenSize = 2;
			break;
		case IntSpb:
			if (length != 4)
			{
				m.printf("attempt to store %d bytes in a clumplet, need 4", length);
			}
			break;
		case BigIntSpb:
			if (length != 8)
			{
				m.printf("attempt to store %d bytes in a clumplet, need 8", length);
			}
			break;
		case ByteSpb:
			if (length != 1)
			{
				m.printf("attempt to store %d bytes in a clumplet, need 1", length);
			}
			break;
		default:
			invalid_structure("unknown clumplet type", t);
		}

		if (m.isEmpty())
		{
			// OK, no errors
			break;
		}

		if (!upgradeVersion())
		{
			// can't upgrade - report failure
			usage_mistake(m.c_str());
			return;
		}
	}

	// Check that resulting data doesn't overflow size limit
	size_overflow(dynamic_buffer.getCount() + length + lenSize + 1 > sizeLimit);

	// Insert the data
	const FB_SIZE_T saved_offset = cur_offset;
	dynamic_buffer.insert(cur_offset++, tag);
	switch (lenSize)
	{
	case 0:
		break;
	case 1:
		dynamic_buffer.insert(cur_offset++, static_cast<UCHAR>(length));
		break;
	case 2:
		{
			UCHAR b[2];
			toVaxInteger(b, sizeof(b), length);
			dynamic_buffer.insert(cur_offset, b, sizeof(b));
			cur_offset += 2;
		}
		break;
	case 4:
		{
			UCHAR b[4];
			toVaxInteger(b, sizeof(b), length);
			dynamic_buffer.insert(cur_offset, b, sizeof(b));
			cur_offset += 4;
		}
		break;
	default:
		fb_assert(lenSize >= 0 && lenSize <= 2 || lenSize == 4);
		return;
	}
	dynamic_buffer.insert(cur_offset, static_cast<const UCHAR*>(bytes), length);
	const FB_SIZE_T new_offset = cur_offset + length;
	cur_offset = saved_offset;
    adjustSpbState();
	cur_offset = new_offset;
}

void ClumpletWriter::insertTag(UCHAR tag)
{
	insertBytesLengthCheck(tag, 0, 0);
}


void ClumpletWriter::insertEndMarker(UCHAR tag)
{
	// Check that we're not beyond the end of buffer.
	// We get there when we set end marker.
	if (cur_offset > dynamic_buffer.getCount())
	{
		usage_mistake("write past EOF");
		return;
	}

	// Check that resulting data doesn't overflow size limit
	size_overflow(cur_offset + 1 > sizeLimit);

	dynamic_buffer.shrink(cur_offset);
	dynamic_buffer.push(tag);

	cur_offset += 2; // Go past EOF to indicate we set the marker
}

void ClumpletWriter::deleteClumplet()
{
	const UCHAR* clumplet = getBuffer() + cur_offset;
	const UCHAR* buffer_end = getBufferEnd();

	// Check for EOF
	if (clumplet >= buffer_end)
	{
		usage_mistake("write past EOF");
		return;
	}

	if (buffer_end - clumplet < 2)
	{
		// It appears we're erasing EOF marker
		dynamic_buffer.shrink(cur_offset);
	}
	else {
		dynamic_buffer.removeCount(cur_offset, getClumpletSize(true, true, true));
	}
}

bool ClumpletWriter::deleteWithTag(UCHAR tag)
{
   bool rc = false;
   while (find(tag))
   {
       rc = true;
       deleteClumplet();
   }

   return rc;
}

bool ClumpletWriter::upgradeVersion()
{
	// Sanity check
	if (!kindList)
	{
		return false;
	}

	// Check for required version - use highmost one
	const KindList* newest = kindList;
	for (const KindList* itr = kindList; itr->tag != EndOfList; ++itr)
	{
		if (itr->tag > newest->tag)
		{
			newest = itr;
		}
	}

	if (getBufferLength() && newest->tag <= getBufferTag())
	{
		return false;
	}

	// Copy data to new clumplet writer
	FB_SIZE_T newPos = 0;
	ClumpletWriter newPb(newest->kind, sizeLimit, newest->tag);
	const FB_SIZE_T currentPosition = cur_offset;
	for(rewind(); !isEof(); moveNext())
	{
		if (currentPosition == cur_offset)
		{
			newPos = newPb.cur_offset;
		}
		newPb.insertClumplet(getClumplet());
		newPb.moveNext();
	}

	// Return it to current clumplet writer in new format
	dynamic_buffer.clear();
	kind = newest->kind;
	dynamic_buffer.push(newPb.dynamic_buffer.begin(), newPb.dynamic_buffer.getCount());

	// Set pointer to correct position
	if (newPos)
	{
		cur_offset = newPos;
	}
	else
	{
		rewind();
	}

	return true;
}

void ClumpletWriter::insertClumplet(const SingleClumplet& clumplet)
{
	insertBytes(clumplet.tag, clumplet.data, clumplet.size);
}

void ClumpletWriter::clear()
{
	reset(isTagged() ? getBufferTag() : 0);
}

} // namespace
