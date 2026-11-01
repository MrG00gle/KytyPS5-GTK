#include "launcher/gtk/trophyPresentation.h"

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/launcherContext.h"

#include <cstdarg>
#include <string_view>

namespace kyty::gtk {

namespace {

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

const char* GradeClass(core::TrophyGrade grade) {
	switch (grade) {
		case core::TrophyGrade::Platinum: return "trophy-platinum";
		case core::TrophyGrade::Gold: return "trophy-gold";
		case core::TrophyGrade::Silver: return "trophy-silver";
		case core::TrophyGrade::Bronze: return "trophy-bronze";
	}
	return "trophy-bronze";
}

} // namespace

const char* TrophyGradeLabel(core::TrophyGrade grade) {
	switch (grade) {
		// TRANSLATORS: a trophy grade.
		case core::TrophyGrade::Platinum: return C_("trophy grade", "Platinum");
		// TRANSLATORS: a trophy grade.
		case core::TrophyGrade::Gold: return C_("trophy grade", "Gold");
		// TRANSLATORS: a trophy grade.
		case core::TrophyGrade::Silver: return C_("trophy grade", "Silver");
		// TRANSLATORS: a trophy grade.
		case core::TrophyGrade::Bronze: return C_("trophy grade", "Bronze");
	}
	return C_("trophy grade", "Bronze");
}

GtkWidget* NewTrophyGradeIcon(core::TrophyGrade grade, int pixel_size) {
	GtkWidget* icon = gtk_image_new_from_icon_name("kyty-trophy-symbolic");
	gtk_image_set_pixel_size(GTK_IMAGE(icon), pixel_size);
	gtk_widget_add_css_class(icon, "trophy-grade-icon");
	gtk_widget_add_css_class(icon, GradeClass(grade));
	gtk_accessible_update_property(GTK_ACCESSIBLE(icon), GTK_ACCESSIBLE_PROPERTY_LABEL,
	                               TrophyGradeLabel(grade), -1);
	return icon;
}

GtkWidget* NewTrophyGradeCount(core::TrophyGrade grade, std::uint32_t count, int icon_size) {
	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_add_css_class(box, "trophy-grade-count");
	GtkWidget* icon = NewTrophyGradeIcon(grade, icon_size);
	// The box carries the label; the icon alone would repeat the grade.
	gtk_accessible_update_state(GTK_ACCESSIBLE(icon), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
	gtk_box_append(GTK_BOX(box), icon);
	GtkWidget* number = gtk_label_new(std::to_string(count).c_str());
	gtk_box_append(GTK_BOX(box), number);
	const std::string text =
	    // TRANSLATORS: a trophy count for screen readers: "<grade>: <number of trophies>".
	    Format(C_("trophy count", "%s: %u"), TrophyGradeLabel(grade), static_cast<unsigned>(count));
	gtk_widget_set_tooltip_text(box, text.c_str());
	gtk_accessible_update_property(GTK_ACCESSIBLE(box), GTK_ACCESSIBLE_PROPERTY_LABEL, text.c_str(),
	                               -1);
	return box;
}

std::string TrophyEarnedText(std::int64_t unix_ms) {
	// Whole seconds, rounded down, as the Qt launcher's QDateTime format without milliseconds.
	const std::int64_t seconds = unix_ms / 1000 - (unix_ms % 1000 < 0 ? 1 : 0);
	GDateTime*         time    = g_date_time_new_from_unix_local(seconds);
	if (time == nullptr) {
		return "";
	}
	GCharPtr text(g_date_time_format(time, "%Y-%m-%d %H:%M:%S"));
	g_date_time_unref(time);
	return std::string(text.View());
}

std::string TrophyPercentText(int percent) {
	// TRANSLATORS: a trophy progress percentage.
	return Format(C_("trophy progress", "%d%%"), percent);
}

GdkTexture* HiddenTrophyTexture() {
	// gdk_texture_new_from_bytes (GTK 4.6), not gdk_texture_new_from_resource (deprecated in 4.20).
	static GdkTexture* const kTexture = []() -> GdkTexture* {
		const std::string path = std::string(kResourceBasePath) + "/images/hidden-trophy.png";
		GErrorPtr         error;
		GBytes*           bytes =
		    g_resources_lookup_data(path.c_str(), G_RESOURCE_LOOKUP_FLAGS_NONE, error.Out());
		GdkTexture* texture =
		    bytes != nullptr ? gdk_texture_new_from_bytes(bytes, error.Out()) : nullptr;
		if (bytes != nullptr) {
			g_bytes_unref(bytes);
		}
		if (texture == nullptr) {
			g_critical("cannot load %s: %s", path.c_str(), std::string(error.Message()).c_str());
		}
		return texture;
	}();
	return kTexture;
}

} // namespace kyty::gtk
