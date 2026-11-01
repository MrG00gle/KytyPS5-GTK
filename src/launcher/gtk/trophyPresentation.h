#pragma once

// What the trophy windows share (task 21.3; plan D42, D46; HC-29): the translated grade names, the
// grade icons and counts of the viewer header and the overview (the Qt launcher's cups, colored
// per grade through style.css), the earned date in local time, the progress text, and the hidden
// trophy image.

#include "launcher/core/trophyModel.h"

#include <cstdint>
#include <gtk/gtk.h>
#include <string>

namespace kyty::gtk {

// The grades in the Qt launcher's display order: Platinum, Gold, Silver, Bronze.
inline constexpr core::TrophyGrade kTrophyGrades[] = {
    core::TrophyGrade::Platinum, core::TrophyGrade::Gold, core::TrophyGrade::Silver,
    core::TrophyGrade::Bronze};

// "Platinum", "Gold", "Silver", or "Bronze", translated (D46: never core::TrophyGradeText).
const char* TrophyGradeLabel(core::TrophyGrade grade);

// The trophy icon (kyty-trophy-symbolic) of pixel_size pixels in the grade's color (the CSS
// classes trophy-platinum, trophy-gold, trophy-silver, trophy-bronze of style.css).
GtkWidget* NewTrophyGradeIcon(core::TrophyGrade grade, int pixel_size);

// The grade's icon followed by count, as the Qt launcher's grade counts; its accessible label is
// "<grade>: <count>".
GtkWidget* NewTrophyGradeCount(core::TrophyGrade grade, std::uint32_t count, int icon_size);

// unix_ms (milliseconds since 1970-01-01 00:00 UTC, TrophyRow::earned_unix_ms) in local time
// (GDateTime in the time zone of TZ or /etc/localtime) as "yyyy-MM-dd HH:mm:ss", the Qt launcher's
// format; "" when GLib cannot represent it.
std::string TrophyEarnedText(std::int64_t unix_ms);

// "<percent>%" (the trophy progress, 0-100).
std::string TrophyPercentText(int percent);

// images/hidden-trophy.png of the resources (transfer none; loaded once, kept for the process;
// nullptr, after a critical, only when the resource cannot be decoded).
GdkTexture* HiddenTrophyTexture();

} // namespace kyty::gtk
