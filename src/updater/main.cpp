#include "updaterCommand.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <vector>

int main(int argc, char* argv[]) {
	try {
		return Kyty::Updater::RunUpdaterCommand(
		    std::vector<std::filesystem::path>(argv, argv + argc));
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
