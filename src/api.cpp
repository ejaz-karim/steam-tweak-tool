// #include <iostream>
// #include <fstream>
// #include <sstream>
// #include <algorithm>
// #include <filesystem>
// #include <string>
// #include "steam-tweak-tool/api.hpp"

// using namespace std;

// Api::Api() {}

// string url = "https://store.steampowered.com/api/appdetails?appids=";

// stringstream Api::removeQuotes(const string &game_ids)
// {
//     stringstream ss(game_ids);
//     string line;
//     stringstream noQuotes;
//     while (getline(ss, line))
//     {
//         line.erase(line.find_first_of('"'), 1);
//         line.erase(line.find_last_of('"'), 1);
//         noQuotes << line << endl;
//     }
//     return noQuotes;
// }
