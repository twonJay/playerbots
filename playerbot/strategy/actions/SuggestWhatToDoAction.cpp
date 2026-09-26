#include "playerbot/playerbot.h"
#include "SuggestWhatToDoAction.h"
#include "playerbot/AiFactory.h"
#include "Chat/ChannelMgr.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotTextMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/ItemVisitors.h"

using namespace ai;

std::map<std::string, int> SuggestWhatToDoAction::factions;

SuggestWhatToDoAction::SuggestWhatToDoAction(PlayerbotAI* ai, std::string name)
    : Action{ ai, name }
    , _locale{ sConfig.GetIntDefault("DBC.Locale", 0 /*LocaleConstant::LOCALE_enUS*/) }
{
    // -- In case we're using auto detect on config file^M
    if (_locale == 255)
        _locale = static_cast<int32>(LocaleConstant::LOCALE_enUS);

    suggestions.push_back(&SuggestWhatToDoAction::instance);
    suggestions.push_back(&SuggestWhatToDoAction::specificQuest);
    suggestions.push_back(&SuggestWhatToDoAction::grindMaterials);
    suggestions.push_back(&SuggestWhatToDoAction::grindReputation);
    suggestions.push_back(&SuggestWhatToDoAction::something);
    suggestions.push_back(&SuggestWhatToDoAction::somethingToxic);
    suggestions.push_back(&SuggestWhatToDoAction::toxicLinks);
    suggestions.push_back(&SuggestWhatToDoAction::thunderfury);
}

namespace
{
    // Vanilla dungeons and raids with the level band players ran them at, the
    // zone(s) the entrance sits in and the faction that could realistically get
    // there (0 = both). Level bands are the usual 1.12 recommendations.
    struct DungeonInfo
    {
        char const* name;
        char const* shortName;
        uint32 minLevel;
        uint32 maxLevel;
        uint32 team;
        std::vector<uint32> zones;
    };

    const std::vector<DungeonInfo> Dungeons =
    {
        { "Ragefire Chasm",              "RFC",      13, 18, HORDE,    { 1637 } },
        { "Wailing Caverns",             "WC",       17, 24, 0,        { 17 } },
        { "The Deadmines",               "Deadmines",17, 26, ALLIANCE, { 40 } },
        { "Shadowfang Keep",             "SFK",      22, 30, 0,        { 130 } },
        { "Blackfathom Deeps",           "BFD",      24, 32, 0,        { 331 } },
        { "The Stockade",                "Stockades",24, 32, ALLIANCE, { 1519 } },
        { "Gnomeregan",                  "Gnomer",   29, 38, ALLIANCE, { 1 } },
        { "Razorfen Kraul",              "RFK",      29, 38, 0,        { 17 } },
        { "Scarlet Monastery Graveyard", "SM GY",    30, 37, 0,        { 85 } },
        { "Scarlet Monastery Library",   "SM Lib",   33, 40, 0,        { 85 } },
        { "Scarlet Monastery Armory",    "SM Arm",   36, 42, 0,        { 85 } },
        { "Scarlet Monastery Cathedral", "SM Cath",  38, 45, 0,        { 85 } },
        { "Razorfen Downs",              "RFD",      37, 46, 0,        { 17, 400 } },
        { "Uldaman",                     "Uldaman",  41, 51, 0,        { 3 } },
        { "Zul'Farrak",                  "ZF",       44, 54, 0,        { 440 } },
        { "Maraudon",                    "Mara",     46, 55, 0,        { 405 } },
        { "the Sunken Temple",           "ST",       50, 60, 0,        { 8 } },
        { "Blackrock Depths",            "BRD",      52, 60, 0,        { 25, 51, 46 } },
        { "Lower Blackrock Spire",       "LBRS",     55, 60, 0,        { 25, 51, 46 } },
        { "Upper Blackrock Spire",       "UBRS",     58, 60, 0,        { 25, 51, 46 } },
        { "Dire Maul East",              "DM East",  55, 60, 0,        { 357 } },
        { "Dire Maul West",              "DM West",  57, 60, 0,        { 357 } },
        { "Dire Maul North",             "DM North", 57, 60, 0,        { 357 } },
        { "Scholomance",                 "Scholo",   58, 60, 0,        { 28 } },
        { "Stratholme",                  "Strat",    58, 60, 0,        { 139 } },
        { "Molten Core",                 "MC",       60, 60, 0,        { 25, 51, 46 } },
        { "Onyxia's Lair",               "Ony",      60, 60, 0,        { 15 } },
        { "Zul'Gurub",                   "ZG",       60, 60, 0,        { 33 } },
        { "Blackwing Lair",              "BWL",      60, 60, 0,        { 25, 51, 46 } },
    };

    bool IsHomeCapital(uint32 zoneId, uint32 team)
    {
        switch (zoneId)
        {
            case 1519: case 1537: case 1657: return team == ALLIANCE;   // Stormwind, Ironforge, Darnassus
            case 1637: case 1638: case 1497: return team == HORDE;      // Orgrimmar, Thunder Bluff, Undercity
            default: return false;
        }
    }

    // local: level-appropriate dungeons whose entrance is in the bot's zone
    // reachable: every level-appropriate dungeon the bot's faction would run
    void CollectDungeons(Player* bot, std::vector<DungeonInfo const*>& local, std::vector<DungeonInfo const*>& reachable)
    {
        uint32 const zoneId = bot->GetZoneId();
        uint32 const level = bot->GetLevel();
        uint32 const team = bot->GetTeam();

        for (DungeonInfo const& dungeon : Dungeons)
        {
            if (level < dungeon.minLevel || level > dungeon.maxLevel)
                continue;
            if (dungeon.team && dungeon.team != team)
                continue;

            reachable.push_back(&dungeon);
            if (std::find(dungeon.zones.begin(), dungeon.zones.end(), zoneId) != dungeon.zones.end())
                local.push_back(&dungeon);
        }
    }

    // Groups form in the capitals and at the dungeon's doorstep; out in the world
    // people are busy questing and rarely ask.
    const uint32 LFG_ELSEWHERE_CHANCE = 10;
    // At a hub, this share of suggestions is LFG talk instead of a random pick.
    const uint32 LFG_HUB_CHANCE = 40;
}

bool SuggestWhatToDoAction::isUseful()
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) || bot->GetGroup() || bot->GetInstanceId())
        return false;

    std::string qualifier = "suggest what to do";
    time_t lastSaid = AI_VALUE2(time_t, "last said", qualifier);
    return (time(0) - lastSaid) > 30;
}

bool SuggestWhatToDoAction::Execute(Event& event)
{
    std::vector<DungeonInfo const*> local, reachable;
    CollectDungeons(bot, local, reachable);
    bool const atGroupHub = !local.empty() || (!reachable.empty() && IsHomeCapital(bot->GetZoneId(), bot->GetTeam()));

    if (atGroupHub && urand(1, 100) <= LFG_HUB_CHANCE)
        instance();
    else
    {
        int index = rand() % suggestions.size();
        (this->*suggestions[index])();
    }

    std::string qualifier = "suggest what to do";
    time_t lastSaid = AI_VALUE2(time_t, "last said", qualifier);
    ai->GetAiObjectContext()->GetValue<time_t>("last said", qualifier)->Set(time(0) + urand(1, 60));

    return true;
}

void SuggestWhatToDoAction::instance()
{
    std::vector<DungeonInfo const*> local, reachable;
    CollectDungeons(bot, local, reachable);

    std::vector<DungeonInfo const*> const* pool = nullptr;
    if (!local.empty())
        pool = &local;
    else if (IsHomeCapital(bot->GetZoneId(), bot->GetTeam()))
        pool = &reachable;
    else if (urand(1, 100) <= LFG_ELSEWHERE_CHANCE)
        pool = &reachable;

    if (!pool || pool->empty())
        return;

    DungeonInfo const* dungeon = (*pool)[urand(0, pool->size() - 1)];
    BroadcastHelper::BroadcastSuggestInstance(ai, dungeon->name, dungeon->shortName, bot);
}

std::vector<uint32> SuggestWhatToDoAction::GetIncompletedQuests()
{
    std::vector<uint32> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
            result.push_back(questId);
    }

    return result;
}

void SuggestWhatToDoAction::specificQuest()
{
    std::vector<uint32> quests = GetIncompletedQuests();
    if (quests.empty())
        return;

    BroadcastHelper::BroadcastSuggestQuest(ai, quests, bot);
}

void SuggestWhatToDoAction::grindMaterials()
{
    if (bot->GetLevel() <= 5)
        return;

    auto vec = ItemUsageValue::GetAllReagentItemIdsForCraftingSkillsVector();

    if (vec.size() > 0)
    {
        uint32 randomItemId = vec[urand() % vec.size()];

        const ItemPrototype* proto = ObjectMgr::GetItemPrototype(randomItemId);
        if (proto)
        {
            BroadcastHelper::BroadcastSuggestGrindMaterials(ai, ai->GetChatHelper()->formatItem(proto), bot);
        }
    }

    return;
}

void SuggestWhatToDoAction::grindReputation()
{
    if (factions.empty())
    {
        factions["Argent Dawn"] = 60;
        factions["Bloodsail Buccaneers"] = 40;
        factions["Brood of Nozdormu"] = 60;
        factions["Cenarion Circle"] = 55;
        factions["Darkmoon Faire"] = 20;
        factions["Hydraxian Waterlords"] = 60;
        factions["Ravenholdt"] = 20;
        factions["Thorium Brotherhood"] = 40;
        factions["Timbermaw Hold"] = 50;
        factions["Wintersaber Trainers"] = 50;
        factions["Booty Bay"] = 30;
        factions["Everlook"] = 40;
        factions["Gadgetzan"] = 50;
        factions["Ratchet"] = 20;

#ifndef MANGOSBOT_ZERO
        factions["Ashtongue Deathsworn"] = 70;
        factions["Cenarion Expedition"] = 62;
        factions["The Consortium"] = 65;
        factions["Honor Hold"] = 66;
        factions["Keepers of Time"] = 68;
        factions["Netherwing"] = 65;
        factions["Ogri'la"] = 65;
        factions["The Scale of the Sands"] = 65;
        factions["Sporeggar"] = 65;
        factions["Tranquillien"] = 10;
        factions["The Violet Eye"] = 70;
#endif

#ifdef MANGOSBOT_TWO
        factions["Argent Crusade"] = 75;
        factions["Ashen Verdict"] = 75;
        factions["The Kalu'ak"] = 72;
        factions["Kirin Tor"] = 75;
        factions["Knights of the Ebon Blade"] = 77;
        factions["The Sons of Hodir"] = 78;
        factions["The Wyrmrest Accord"] = 77;
#endif
    }

    std::vector<std::string> levels;
    levels.push_back("honored");
    levels.push_back("revered");
    levels.push_back("exalted");

    std::vector<std::string> allowedFactions;
    for (std::map<std::string, int>::iterator i = factions.begin(); i != factions.end(); ++i) {
        if ((int)bot->GetLevel() >= i->second) allowedFactions.push_back(i->first);
    }

    if (allowedFactions.empty()) return;

    BroadcastHelper::BroadcastSuggestGrindReputation(ai, levels, allowedFactions, bot);
}

void SuggestWhatToDoAction::something()
{
    BroadcastHelper::BroadcastSuggestSomething(ai, bot);
}

void SuggestWhatToDoAction::somethingToxic()
{
    BroadcastHelper::BroadcastSuggestSomethingToxic(ai, bot);
}

void SuggestWhatToDoAction::toxicLinks()
{
    BroadcastHelper::BroadcastSuggestToxicLinks(ai, bot);
}

void SuggestWhatToDoAction::thunderfury()
{
    BroadcastHelper::BroadcastSuggestThunderfury(ai, bot);
}

class FindTradeItemsVisitor : public IterateItemsVisitor
{
public:
    FindTradeItemsVisitor(uint32 quality) : quality(quality), IterateItemsVisitor() {}

    virtual bool Visit(Item* item)
    {
        ItemPrototype const* proto = item->GetProto();
        if (proto->Quality != quality)
            return true;

        if (proto->Class == ITEM_CLASS_TRADE_GOODS && proto->Bonding == NO_BIND)
        {
            if(proto->Quality == ITEM_QUALITY_NORMAL && item->GetCount() > 1 && item->GetCount() == item->GetMaxStackCount())
                stacks.push_back(proto->ItemId);

            items.push_back(proto->ItemId);
            count[proto->ItemId] += item->GetCount();
        }

        return true;
    }

    std::map<uint32, int > count;
    std::vector<uint32> stacks;
    std::vector<uint32> items;

private:
    uint32 quality;
};

bool SuggestTradeAction::isUseful()
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) || bot->GetGroup() || bot->GetInstanceId())
        return false;

    return true;
}

bool SuggestTradeAction::Execute(Event& event)
{
    uint32 quality = urand(0, 100);
    if (quality > 95)
        quality = ITEM_QUALITY_LEGENDARY;
    else if (quality > 90)
        quality = ITEM_QUALITY_EPIC;
    else if (quality > 75)
        quality = ITEM_QUALITY_RARE;
    else if (quality > 50)
        quality = ITEM_QUALITY_UNCOMMON;
    else
        quality = ITEM_QUALITY_NORMAL;

    uint32 item = 0, count = 0;
    while (quality-- > ITEM_QUALITY_POOR)
    {
        FindTradeItemsVisitor visitor(quality);
        ai->InventoryIterateItems(&visitor, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
        if (!visitor.stacks.empty())
        {
            int index = urand(0, visitor.stacks.size() - 1);
            item = visitor.stacks[index];
        }

        if (!item)
        {
            if (!visitor.items.empty())
            {
                int index = urand(0, visitor.items.size() - 1);
                item = visitor.items[index];
            }
        }

        if (item)
        {
            count = visitor.count[item];
            break;
        }
    }

    if (!item || !count)
        return false;

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(item);
    if (!proto)
        return false;

    uint32 price = ItemUsageValue::GetBotSellPrice(proto, bot) * count;
    if (!price)
        return false;

    BroadcastHelper::BroadcastSuggestSell(ai, proto, count, price, bot);

    return true;
}