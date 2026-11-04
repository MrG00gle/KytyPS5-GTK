#pragma once

// Translation helpers of the GTK launcher (task 19.2, Requirement 6.12; plan D10). Every
// user-visible string goes through _(), N_(), C_(), or NGettext() in the kyty-launcher text domain;
// GtkBuilder files use <interface domain="kyty-launcher"> and translatable="yes". Every file with
// such strings is listed in po/POTFILES.in, and the update-pot target extracts them.
//
// GETTEXT_PACKAGE is defined before <glib/gi18n-lib.h>, so _() always looks up the kyty-launcher
// domain, whatever the process's default domain is. main.cpp binds the domain to a directory.

#define GETTEXT_PACKAGE "kyty-launcher"

#include <glib/gi18n-lib.h>

namespace kyty::gtk {

// The singular or plural form for n in the kyty-launcher domain (g_dngettext). xgettext extracts
// it with --keyword=NGettext:1,2.
inline const char* NGettext(const char* singular, const char* plural, unsigned long n) {
	return g_dngettext(GETTEXT_PACKAGE, singular, plural, n);
}

// The two untranslated forms of a plural text, for a table that NGettext() translates later (what
// N_() is to _()). xgettext extracts them with --keyword=NGettextNoop:1,2.
struct PluralText {
	const char* singular = nullptr;
	const char* plural   = nullptr; // nullptr: a text without a plural form
};

constexpr PluralText NGettextNoop(const char* singular, const char* plural) {
	return {singular, plural};
}

} // namespace kyty::gtk
