#include "backup.h"

unsigned text_add(char *out, unsigned at, const char *value, unsigned limit)
{
    while (*value && at < limit)
        out[at++] = *value++;
    out[at] = '\0';
    return at;
}

unsigned text_number(char *out, unsigned at, uint32_t number)
{
    uint32_t place = 1000000000;
    int started = 0;
    const uint32_t places[] = {100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
    for (unsigned i = 0; i < 10; ++i) {
        unsigned digit = 0;
        while (number >= place) {
            number -= place;
            ++digit;
        }
        if (started || digit || i == 9) {
            out[at++] = (char)('0' + digit);
            started = 1;
        }
        if (i < 9)
            place = places[i];
    }
    out[at] = '\0';
    return at;
}

int numeric_id(const char *id)
{
    for (; *id; ++id)
        if (*id < '0' || *id > '9')
            return 0;
    return 1;
}

static int id_is(const char *id, const char *expected)
{
    while (*id && *id == *expected) {
        ++id;
        ++expected;
    }
    return *id == *expected;
}

const char *game_name(const char *id)
{
    if (!numeric_id(id)) {
        if (id_is(id, "aviaozinho_zeebo")) return "Aviãozinho";
        if (id_is(id, "sm64_zeebo")) return "Super Mario 64 Zeebo Edition";
        if (id_is(id, "opentyrian_zeebo")) return "Tyrian";
        if (id_is(id, "zeesnes")) return "ZeeSnes";
        if (id_is(id, "zeetris")) return "Zeetris";
        return NULL;
    }
    unsigned number = 0;
    for (unsigned i = 0; id[i]; ++i) {
        if (i == 6)
            return NULL;
        number = number * 10 + (unsigned)(id[i] - '0');
    }
    // Nomes de referência do catálogo Jogos e aplicativos do OpenZeebo.
    switch (number) {
    case 12875: return "IMICRO3D";
    case 263019: return "Ultimate Chess 3D";
    case 274214: return "Crash Bandicoot Nitro Kart 3D";
    case 274259: return "Action Hero 3D: Wild Dog";
    case 274754: return "Double Dragon";
    case 274755: return "Z-Wheel";
    case 274791: return "Zeebo App";
    case 274802: return "Quake";
    case 274803: return "FIFA 09";
    case 274804: return "Treino Cerebral";
    case 276121: return "Need for Speed Carbon";
    case 276151: return "Alpine Racer";
    case 276152: return "Ridge Racer";
    case 276153: return "Quake II";
    case 276154: return "Prey Evil";
    case 276212: return "Pac-Mania";
    case 276675: return "Resident Evil 4";
    case 276731: return "Tekken 2";
    case 276809: return "Zeebo Extreme Rolimã";
    case 277083: return "Bejeweled Twist";
    case 277229: return "Zeebo Family Pack";
    case 277285: return "Zeebo Extreme Corrida Aérea";
    case 277380: return "Galaxy on Fire";
    case 277455: return "Zenonia";
    case 277495: return "Zeebo Channels";
    case 277534: return "Zeebo Sports Tênis";
    case 277727: return "Zeebo Extreme Baja";
    case 278200: return "Heavy Weapon";
    case 278212: return "Zeebo Sports Vôlei";
    case 278282: return "Rally Master Pro";
    case 278283: return "Zeebo Extreme Jetboard";
    case 278285: return "Zeebo Extreme Bóia Cross";
    case 278738: return "Zeebo Sports Queimada";
    case 278962: return "Peggle";
    case 278965: return "Toy Raid";
    case 278986: return "Caveman Ninja";
    case 278987: return "Spin Master";
    case 278988: return "Street Hoop";
    case 279036: return "Um Jogo de Ovos";
    case 279125: return "Super BurgerTime";
    case 279126: return "Karnov's Revenge";
    case 279159: return "Zeebo Sports Peteca";
    case 279173: return "Wizard Fire";
    case 279200: return "Magical Drop III";
    case 279233: return "Dark Seal";
    case 279369: return "Alien Breaker Deluxe";
    case 279380: return "Zeebo F.C. Foot Camp";
    case 279382: return "Zeeboids";
    case 279394: return "Zeebo Clube";
    case 279712: return "Zuma's Revenge";
    case 279888: return "Bad Dudes vs. DragonNinja";
    case 279889: return "Heavy Barrel";
    case 279891: return "Ghouls'n Ghosts";
    case 279892: return "Captain Commando";
    case 279893: return "Street Fighter II";
    case 279894: return "Super Street Fighter II";
    case 279895: return "Alien vs. Predator";
    case 279897: return "Strider";
    case 279911: return "Three Wonders";
    case 280173: return "Disney All Star Cards";
    case 280214: return "Armageddon Squadron";
    case 280221: return "Iron Sight";
    case 280238: return "Powerboat Challenge";
    case 280386: return "Alice no País das Maravilhas";
    case 280394: return "Reckless Racing";
    case 280463: return "Tork and Kral";
    case 280602: return "Raging Thunder 2";
    case 280634: return "Turma da Mônica";
    case 280647: return "Zeebo F.C. Super League";
    case 555001: return "Street Fighter Alpha";
    default: return NULL;
    }
}
