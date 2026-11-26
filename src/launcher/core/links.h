#pragma once

// External pages the launcher links to (task 16.2; plan D23).

namespace kyty::core {

// The HEN Cheats Collection repository: the cheats dialog's credit link opens it (Requirement 11.4;
// Qt's "Cheats by TeeKay87 / HEN Cheats Collection and its contributors." label).
inline constexpr char kHenCheatsRepository[] = "https://github.com/TeeKay87/HEN-Cheats-Collection";

// The KytyPS5-Modules repository: the cheats dialog's other credit link (upstream KytyPS5 commit
// d2ec91b8: "Patches and cheats by KytyPS5-Modules and TeeKay87 / HEN Cheats Collection and their
// contributors.").
inline constexpr char kKytyModulesRepository[] = "https://github.com/KytyPS5/KytyPS5-Modules";

} // namespace kyty::core
