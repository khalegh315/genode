/*
 * \brief  Connection to timer service and timeout scheduler
 * \author Martin Stein
 * \date   2016-11-04
 */

/*
 * Copyright (C) 2016-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <timer_session/connection.h>
#include <base/internal/globals.h>

using namespace Genode;
using namespace Genode::Trace;


void Timer::Connection::_update_real_time()
{
	Lock_guard<Lock> lock_guard(_real_time_lock);


	/*
	 * Update timestamp, time, and real-time value
	 */

	Timestamp     ts         = 0UL;
	unsigned long us         = 0UL;
	unsigned long latency_us = ~0UL;

	/*
	 * We retry reading out timestamp plus remote time until the result
	 * fulfills a given latency. If the maximum number of trials is
	 * reached, we take the results that has the lowest latency.
	 */
	for (unsigned remote_time_trials = 0;
	     remote_time_trials < MAX_REMOTE_TIME_TRIALS;
	     remote_time_trials++)
	{
		/* read out the two time values close in succession */
		Timestamp     volatile new_ts = _timestamp();
		unsigned long volatile new_us = elapsed_us();

		/*
		 * If interpolation is not ready, yet we cannot determine a latency
		 * and take the values as they are.
		 */
		if (_interpolation_quality < MAX_INTERPOLATION_QUALITY) {
			us = new_us;
			ts = new_ts;
			break;
		}
		/* determine latency between reading out timestamp and time value */
		Timestamp     const ts_diff        = _timestamp() - new_ts;
		unsigned long const new_latency_us =
			_ts_to_us_ratio(ts_diff, _us_to_ts_factor, _us_to_ts_factor_shift);

		/* remember results if the latency was better than on the last trial */
		if (new_latency_us < latency_us) {
			us = new_us;
			ts = new_ts;

			/* take the results if the latency fulfills the given maximum */
			if (latency_us < MAX_REMOTE_TIME_LATENCY_US) {
				break;
			}
		}
	}

	/* determine timestamp and time difference */
	unsigned long const us_diff = us - _us;
	Timestamp           ts_diff = ts - _ts;

	/* overwrite timestamp, time, and real time member */
	_us         = us;
	_ts         = ts;
	_real_time += Microseconds(us_diff);


	/*
	 * Update timestamp-to-time factor and its shift
	 */

	unsigned      factor_shift        = _us_to_ts_factor_shift;
	unsigned long old_factor          = _us_to_ts_factor;
	Timestamp     max_ts_diff         = ~(Timestamp)0ULL >> factor_shift;
	Timestamp     min_ts_diff_shifted = ~(Timestamp)0ULL >> 1;

	/*
	 * If the calculation type is bigger than the resulting factor type,
	 * we have to apply further limitations to avoid a loss at the final cast.
	 */
	if (sizeof(Timestamp) > sizeof(unsigned long)) {

		Timestamp       limit_ts_diff_shifted = (Timestamp)~0UL * us_diff;
		Timestamp const limit_ts_diff         = limit_ts_diff_shifted >>
		                                        factor_shift;

		/*
		 * Avoid that we leave the factor shift on such a high level that
		 * casting the factor to its final type causes a loss.
		 */
		if (max_ts_diff > limit_ts_diff) {
			max_ts_diff = limit_ts_diff;
		}
		/*
		 * Avoid that we raise the factor shift such that casting the factor
		 * to its final type causes a loss.
		 */
		limit_ts_diff_shifted >>= 1;
		if (min_ts_diff_shifted > limit_ts_diff_shifted) {
			min_ts_diff_shifted = limit_ts_diff_shifted;
		}
	}

	struct Factor_update_failed : Genode::Exception { };
	try {
		/* meet the timestamp-difference limit before applying the shift */
		while (ts_diff > max_ts_diff) {

			/* if possible, lower the shift to meet the limitation */
			if (!factor_shift) {
				error("timestamp difference too big");
				throw Factor_update_failed();
			}
			factor_shift--;
			max_ts_diff = (max_ts_diff << 1) | 1;
			old_factor >>= 1;
		}
		/*
		 * Apply current shift to timestamp difference and try to even
		 * raise the shift successively to get as much precision as possible.
		 */
		Timestamp ts_diff_shifted = ts_diff << factor_shift;
		while (ts_diff_shifted < us_diff << MIN_FACTOR_LOG2)
		{
			factor_shift++;
			ts_diff_shifted <<= 1;
			old_factor <<= 1;
		}
		/*
		 * The cast to unsigned long does not cause a loss because of the
		 * limitations we applied to the timestamp difference.
		 */

		unsigned long const new_factor =
			(unsigned long)((Timestamp)ts_diff_shifted / us_diff);

		/* update interpolation-quality value */
		if (old_factor > new_factor) { _update_interpolation_quality(new_factor, old_factor); }
		else                         { _update_interpolation_quality(old_factor, new_factor); }

		/* overwrite factor and factor-shift member */
		_us_to_ts_factor_shift = factor_shift;
		_us_to_ts_factor       = new_factor;

	} catch (Factor_update_failed) {

		/* disable interpolation */
		_interpolation_quality = 0;
	}
}


Duration Timer::Connection::curr_time()
{
	_enable_modern_mode();

	Reconstructible<Lock_guard<Lock> > lock_guard(_real_time_lock);
	Duration                           interpolated_time(_real_time);

	/*
	 * Interpolate with timestamps only if the factor value
	 * remained stable for some time. If we would interpolate with
	 * a yet unstable factor, there's an increased risk that the
	 * interpolated time falsely reaches an enourmous level. Then
	 * the value would stand still for quite some time because we
	 * can't let it jump back to a more realistic level.
	 */
	if (_interpolation_quality == MAX_INTERPOLATION_QUALITY)
	{
		/* buffer interpolation related members and free the lock */
		Timestamp     const ts                    = _ts;
		unsigned long const us_to_ts_factor       = _us_to_ts_factor;
		unsigned      const us_to_ts_factor_shift = _us_to_ts_factor_shift;

		lock_guard.destruct();

		/* interpolate time difference since the last real time update */
		Timestamp     const ts_diff = _timestamp() - ts;
		unsigned long const us_diff = _ts_to_us_ratio(ts_diff, us_to_ts_factor,
		                                              us_to_ts_factor_shift);

		interpolated_time += Microseconds(us_diff);

	} else {

		/* use remote timer instead of timestamps */
		interpolated_time += Microseconds(elapsed_us() - _us);

		lock_guard.destruct();
	}
	return _update_interpolated_time(interpolated_time);
}