/*
 *	PROGRAM:		Client/Server Common Code
 *	MODULE:			fb_atomic.h
 *	DESCRIPTION:	Atomic counters
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
 */

#ifndef CLASSES_FB_ATOMIC_H
#define CLASSES_FB_ATOMIC_H

#include <atomic>

namespace Firebird {

class AtomicCounter
{
public:
	typedef IPTR counter_type;

	explicit AtomicCounter(counter_type value = 0) noexcept
		: counter(value)
	{
	}

	~AtomicCounter()
	{
	}

	counter_type value() const noexcept { return counter.load(std::memory_order_acquire); }

	counter_type exchangeAdd(counter_type value) noexcept
	{
		return counter.fetch_add(value);
	}

	void setValue(counter_type val) noexcept
	{
		counter.store(val, std::memory_order_release);
	}

	bool compareExchange(counter_type oldVal, counter_type newVal) noexcept
	{
		return counter.compare_exchange_strong(oldVal, newVal);
	}

	// returns old value
	counter_type exchangeBitAnd(counter_type val) noexcept
	{
		while (true)
		{
			counter_type oldVal = counter.load();
			if (counter.compare_exchange_strong(oldVal, oldVal & val))
				return oldVal;
		}
	}

	// returns old value
	counter_type exchangeBitOr(counter_type val) noexcept
	{
		while (true)
		{
			counter_type oldVal = counter.load();
			if (counter.compare_exchange_strong(oldVal, oldVal | val))
				return oldVal;
		}
	}

	// returns old value
	counter_type exchangeGreater(counter_type val) noexcept
	{
		while (true)
		{
			counter_type oldVal = counter.load(std::memory_order_acquire);

			if (oldVal >= val)
				return oldVal;

			if (counter.compare_exchange_strong(oldVal, val))
				return oldVal;
		}
	}

	// returns old value
	counter_type exchangeLower(counter_type val) noexcept
	{
		while (true)
		{
			counter_type oldVal = counter.load(std::memory_order_acquire);

			if (oldVal <= val)
				return oldVal;

			if (counter.compare_exchange_strong(oldVal, val))
				return oldVal;
		}
	}

	void operator &=(counter_type val) noexcept
	{
		counter &= val;
	}

	void operator |=(counter_type val) noexcept
	{
		counter |= val;
	}

	// returns new value !
	counter_type operator ++() noexcept
	{
		return counter++ + 1;
	}

	// returns new value !
	counter_type operator --() noexcept
	{
		return counter-- - 1;
	}

	inline operator counter_type () const noexcept
	{
		return value();
	}

	inline void operator =(counter_type val) noexcept
	{
		setValue(val);
	}

	inline counter_type operator +=(counter_type val) noexcept
	{
		return exchangeAdd(val) + val;
	}

	inline counter_type operator -=(counter_type val) noexcept
	{
		return exchangeAdd(-val) - val;
	}

private:
	std::atomic<counter_type> counter;

	static_assert(sizeof(counter_type) == sizeof(counter), "Internal and external counter sizes need to match");
};

} // namespace Firebird

#endif // CLASSES_FB_ATOMIC_H
