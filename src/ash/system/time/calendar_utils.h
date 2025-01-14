// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_SYSTEM_TIME_CALENDAR_UTILS_H_
#define ASH_SYSTEM_TIME_CALENDAR_UTILS_H_

#include <set>

#include "ash/ash_export.h"
#include "base/time/time.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/geometry/insets.h"

namespace views {

class TableLayout;

}  // namespace views

namespace ash {

namespace calendar_utils {

// Number of days in one week.
constexpr int kDateInOneWeek = 7;

// Milliseconds per minute.
constexpr int kMillisecondsPerMinute = 60000;

// The padding in each date cell view.
constexpr int kDateVerticalPadding = 13;
constexpr int kDateHorizontalPadding = 14;
constexpr int kColumnSetPadding = 5;

// The insets within a Date cell.
constexpr auto kDateCellInsets =
    gfx::Insets::VH(kDateVerticalPadding, kDateHorizontalPadding);

// Duration of opacity animation for visibility changes.
constexpr base::TimeDelta kAnimationDurationForVisibility =
    base::Milliseconds(200);

// Fade-out and fade-in duration for resetting to today animation.
constexpr base::TimeDelta kResetToTodayFadeAnimationDuration =
    base::Milliseconds(100);

// Duration of moving animation.
constexpr base::TimeDelta kAnimationDurationForMoving = base::Milliseconds(300);

// Duration of month moving animation.
constexpr base::TimeDelta kAnimationDurationForMonthMoving =
    base::Milliseconds(600);

// This duration is added to a midnight `base::Time` to adjust the DST when
// adding days by `base::Day`.
//
// For example, in PST time zone, when we add 1 week (7 days) to Nov 3rd 00:00,
// the expected result is Nov 10th 00:00, but the actual result is Nov 9th
// 23:00. Because in Nov 6th is the DST end day and there are 25 hours in that
// day. By adding this time delta, the result is Nov 10th 4:00, which is the
// expected date.
constexpr base::TimeDelta kDurationForAdjustingDST = base::Hours(5);

// Duration subtracted from an existing midnight in UTC to get the
// previous day. It is less than 24 hours to consider daylight savings.
constexpr base::TimeDelta kDurationForGettingPreviousDay = base::Hours(20);

// Event fetch will terminate if we don't receive a response sooner than this.
constexpr base::TimeDelta kEventFetchTimeout = base::Seconds(10);

// Number of months, before and after the month currently on-display, that we
// cache-ahead.
constexpr int kNumSurroundingMonthsCached = 2;

// Maximum number of non-prunable months allowed, which is a function of
// kNumSurroundingMonthsCached.
constexpr int kMaxNumNonPrunableMonths = 2 * kNumSurroundingMonthsCached + 1;

// Maximum number of prunable months to cache. Note that this plus
// kMaxNumNonPrunableMonths is the total maximum number of cached months.
constexpr int kMaxNumPrunableMonths = 20;

// Checks if the `selected_date` is local time today.
bool IsToday(const base::Time selected_date);

// Checks if the two exploded are in the same day.
bool IsTheSameDay(absl::optional<base::Time> date_a,
                  absl::optional<base::Time> date_b);

// Returns the set of months that includes |selected_date| and
// |num_months_out| before and after.
std::set<base::Time> GetSurroundingMonthsUTC(const base::Time& selected_date,
                                             int num_months_out);

// Gets the given `date`'s `Exploded` instance, in UTC time.
base::Time::Exploded GetExplodedUTC(const base::Time& date);

// Gets the `date`'s month name, numeric day of month, and year.
// (e.g. March 10, 2022)
ASH_EXPORT std::u16string GetMonthDayYear(const base::Time date);

// Gets the `date`'s month name, numeric day of month, year and day of week.
// (e.g. Wednesday, May 25, 2022)
ASH_EXPORT std::u16string GetMonthDayYearWeek(const base::Time date);

// Gets the `date`'s month name in string in the current language.
// (e.g. March)
ASH_EXPORT std::u16string GetMonthName(const base::Time date);

// Gets the `date`'s day of month in local format. For some languages, the
// formatter adds some words/characters which means `day` in that language to
// the digital day of month (e.g. 10日).
ASH_EXPORT std::u16string GetDayOfMonth(const base::Time date);

// Gets the `date`'s numeric day of month.
// (e.g. 10)
ASH_EXPORT std::u16string GetDayIntOfMonth(const base::Time local_date);

// Gets the `date`'s month name and the numeric day of month.
// (e.g. March 10)
ASH_EXPORT std::u16string GetMonthNameAndDayOfMonth(const base::Time date);

// Gets the `date`'s hour in twelve hour clock format.
// (e.g. 2:31 AM)
ASH_EXPORT std::u16string GetTwelveHourClockTime(const base::Time date);

// Gets the `date`'s hour in twenty four hour clock format.
// (e.g. 22:31)
ASH_EXPORT std::u16string GetTwentyFourHourClockTime(const base::Time date);

// Gets the `date`'s time zone.
// (e.g. Greenwich Mean Time)
ASH_EXPORT std::u16string GetTimeZone(const base::Time date);

// Gets the index of this day in the week, starts from 1. This number is
// different for different languages.
ASH_EXPORT std::u16string GetDayOfWeek(const base::Time date);

// Gets the `date`'s year.
// (e.g. 2022)
ASH_EXPORT std::u16string GetYear(const base::Time date);

// Gets the `date`'s month name and year.
// (e.g. March 2022)
ASH_EXPORT std::u16string GetMonthNameAndYear(const base::Time date);

// Gets the formatted interval between `start_time` and `end_time` in twelve
// hour clock format.
// (e.g. 8:30 – 9:30 PM or 11:30 AM – 2:30 PM)
ASH_EXPORT std::u16string FormatTwelveHourClockTimeInterval(
    const base::Time& start_time,
    const base::Time& end_time);

// Gets the formatted interval between `start_time` and `end_time` in twenty
// four hour clock format.
// (e.g. 20:30 – 21:30)
ASH_EXPORT std::u16string FormatTwentyFourHourClockTimeInterval(
    const base::Time& start_time,
    const base::Time& end_time);

// Sets up the `TableLayout` to have 7 columns, which is one week row (7 days).
void SetUpWeekColumns(views::TableLayout* layout);

// Computes the distance, in months, between `start_date` and `end_date`.
ASH_EXPORT int GetMonthsBetween(const base::Time& start_date,
                                const base::Time& end_date);

// Gets date with greater value between `d1` and `d2`.
ASH_EXPORT base::Time GetMaxTime(const base::Time d1, const base::Time d2);

// Gets date with lesser value between `d1` and `d2`.
ASH_EXPORT base::Time GetMinTime(const base::Time d1, const base::Time d2);

// Colors.
SkColor GetPrimaryTextColor();
SkColor GetSecondaryTextColor();
SkColor GetDisabledTextColor();

// Get the first day of the month that includes |date|.
ASH_EXPORT base::Time GetFirstDayOfMonth(const base::Time& date);

// Get the first day of the month before the one that includes |date|.
base::Time GetStartOfPreviousMonthLocal(base::Time date);

// Get the first day of the month after the one that includes |date|.
base::Time GetStartOfNextMonthLocal(base::Time date);

// Get UTC midnight on the first day of the month that includes |date|.
base::Time GetStartOfMonthUTC(const base::Time& date);

// Get UTC midnight on the first day of the month before the one that includes
// |date|.
base::Time GetStartOfPreviousMonthUTC(base::Time date);

// Get UTC midnight on the first day of the month after the one that includes
// |date|.
base::Time GetStartOfNextMonthUTC(base::Time date);

// Returns UTC midnight of `date`'s next day without adjusting time difference.
ASH_EXPORT base::Time GetNextDayMidnight(base::Time date);

// Returns true if it's a regular user or the user session is not blocked.
bool IsActiveUser();

// Get the time difference to UTC time based on the time passed in and the
// system timezone. Daylight saving is considered.
ASH_EXPORT base::TimeDelta GetTimeDifference(base::Time date);

// Gets the first day's local midnight of the week based on the `date`.
base::Time GetFirstDayOfWeekLocalMidnight(base::Time date);

// Calculate the start/end times for a fetch of a single month's events.
// `start_of_month_local_midnight` should be local midnight on the first day of
// a month, and the two `base::Time` objects returned are the UTC times that
// represent the start of the month and start of the next month. See the unit
// test of this method in calendar_utils_unittest.cc for examples.
ASH_EXPORT const std::pair<base::Time, base::Time> GetFetchStartEndTimes(
    base::Time start_of_month_local_midnight);

// Gets the int index of this day in the week, starts from 1. This number is
// different for different languages. If cannot find this local's day in a week,
// returns its time exploded's `day_of_week`;
ASH_EXPORT int GetDayOfWeekInt(const base::Time date);

}  // namespace calendar_utils

}  // namespace ash

#endif  // ASH_SYSTEM_TIME_CALENDAR_UTILS_H_
