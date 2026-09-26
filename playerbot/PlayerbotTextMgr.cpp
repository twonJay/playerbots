
#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"

#include "Database/DatabaseEnv.h"
#include "PlayerbotAI.h"

PlayerbotTextMgr::PlayerbotTextMgr()
{
    for (uint8 i = 1; i < MAX_LOCALE; ++i)
    {
        botTextLocalePriority[sObjectMgr.GetStorageLocaleIndexFor(LocaleConstant(i))] = 0;
    }
}

PlayerbotTextMgr::~PlayerbotTextMgr()
{
}

void PlayerbotTextMgr::ReplaceAll(std::string & str, const std::string & from, const std::string & to) {
    if (from.empty())
        return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
    }
}

void PlayerbotTextMgr::ReplacePlaceholders(std::string& text, const std::map<std::string, std::string>& placeholders)
{
    for (auto& placeholder : placeholders)
        ReplaceAll(text, placeholder.first, placeholder.second);
}

void PlayerbotTextMgr::LoadBotTexts()
{
    botTexts.clear();
    sLog.outBasic("Loading playerbot texts...");
    auto results = WorldDatabase.PQuery("SELECT `name`, `text`, `say_type`, `reply_type`, `text_loc1`, `text_loc2`, `text_loc3`, `text_loc4`, `text_loc5`, `text_loc6`, `text_loc7`, `text_loc8` FROM `ai_playerbot_texts`");
    int count = 0;
    if (results)
    {
        do
        {
            std::string text;
            std::map<int32, std::string> text_locale;
            Field* fields = results->Fetch();
            std::string name = fields[0].GetString();
            text = fields[1].GetString();
            uint32 sayType = fields[2].GetUInt32();
            uint32 replyType = fields[3].GetUInt32();
            for (uint8 i = 1; i < MAX_LOCALE; ++i)
            {
                text_locale[sObjectMgr.GetStorageLocaleIndexFor(LocaleConstant(i))] = fields[i + 3].GetString();
            }
            botTexts[name].push_back(BotTextEntry(name, text, text_locale, sayType, replyType));
            count++;
        } while (results->NextRow());
    }
    sLog.outBasic("%d playerbot texts loaded", count);

    // Local chatter kept apart from ai_playerbot_texts, which the playerbots SQL
    // drops and recreates on update. Rows here can be tied to zones, a level band
    // and a faction; see my-content/world/0007_bot_chatter.sql.
    int localCount = 0;
    auto localResults = WorldDatabase.PQuery("SELECT `name`, `text`, `zones`, `min_level`, `max_level`, `team` FROM `my_bot_texts`");
    if (localResults)
    {
        do
        {
            Field* fields = localResults->Fetch();
            BotTextEntry entry(fields[0].GetCppString(), fields[1].GetCppString(), std::map<int32, std::string>(), 0, 0);

            std::stringstream zones(fields[2].GetCppString());
            std::string zone;
            while (std::getline(zones, zone, ','))
                if (uint32 zoneId = std::strtoul(zone.c_str(), nullptr, 10))
                    entry.m_zones.insert(zoneId);

            entry.m_minLevel = fields[3].GetUInt32();
            entry.m_maxLevel = fields[4].GetUInt32();
            switch (fields[5].GetUInt32())
            {
                case 1: entry.m_team = ALLIANCE; break;
                case 2: entry.m_team = HORDE; break;
                default: break;
            }

            botTexts[entry.m_name].push_back(entry);
            ++localCount;
        } while (localResults->NextRow());
    }
    sLog.outBasic("%d local playerbot texts loaded", localCount);
}

void PlayerbotTextMgr::LoadBotTextChance()
{
    if (botTextChance.empty())
    {
        auto results = WorldDatabase.PQuery("SELECT name, probability FROM ai_playerbot_texts_chance");
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                std::string name = fields[0].GetString();
                uint32 probability = fields[1].GetUInt32();

                botTextChance[name] = probability;
            } while (results->NextRow());
        }
    }
}

// general texts

std::string PlayerbotTextMgr::GetBotText(std::string name)
{
    if (botTexts.empty())
    {
        sLog.outError("Can't get bot text %s! No bots texts loaded!", name.c_str());
        return "";
    }
    if (botTexts[name].empty())
    {
        sLog.outDetail("Can't get bot text %s! No bots texts for this name!", name.c_str());
        return "";
    }

    std::vector<BotTextEntry const*> list;
    for (BotTextEntry const& entry : botTexts[name])
        if (!entry.IsRestricted())
            list.push_back(&entry);

    if (list.empty())
        return "";

    BotTextEntry textEntry = *list[urand(0, list.size() - 1)];
    int32 localePrio = GetLocalePriority();
    if (localePrio == -1)
        return textEntry.m_text;
    else
    {
        if (!textEntry.m_text_locales[localePrio].empty())
            return textEntry.m_text_locales[localePrio];
        else
            return textEntry.m_text;
    }
}

std::string PlayerbotTextMgr::GetBotText(std::string name, std::map<std::string, std::string> placeholders)
{
    std::string botText = GetBotText(name);
    if (botText.empty())
        botText = name;

    ReplacePlaceholders(botText, placeholders);

    return botText;
}

// Share of picks that go to lines tied to the bot's current zone whenever any
// qualify, so a handful of zone lines is not drowned out by hundreds of generic ones.
static const uint32 ZONED_TEXT_CHANCE = 50;

std::string PlayerbotTextMgr::GetBotText(std::string name, std::map<std::string, std::string> placeholders, Player* bot)
{
    if (!bot)
        return GetBotText(name, placeholders);

    auto itr = botTexts.find(name);
    if (itr == botTexts.end())
        return "";

    uint32 const zoneId = bot->GetZoneId();
    uint32 const level = bot->GetLevel();
    uint32 const team = bot->GetTeam();

    std::vector<BotTextEntry const*> zoned, general;
    for (BotTextEntry const& entry : itr->second)
    {
        if (!entry.Matches(zoneId, level, team))
            continue;
        (entry.IsZoned() ? zoned : general).push_back(&entry);
    }

    std::vector<BotTextEntry const*>* pool = &general;
    if (!zoned.empty() && (general.empty() || urand(1, 100) <= ZONED_TEXT_CHANCE))
        pool = &zoned;
    if (pool->empty())
        return "";

    BotTextEntry const* textEntry = (*pool)[urand(0, pool->size() - 1)];
    std::string botText = textEntry->m_text;
    int32 localePrio = GetLocalePriority();
    if (localePrio != -1)
    {
        auto loc = textEntry->m_text_locales.find(localePrio);
        if (loc != textEntry->m_text_locales.end() && !loc->second.empty())
            botText = loc->second;
    }

    ReplacePlaceholders(botText, placeholders);
    return botText;
}

// chat replies

std::string PlayerbotTextMgr::GetBotText(ChatReplyType replyType, std::map<std::string, std::string> placeholders)
{
    if (botTexts.empty())
    {
        sLog.outError("Can't get bot text reply %u! No bots texts loaded!", replyType);
        return "";
    }
    if (botTexts["reply"].empty())
    {
        sLog.outDetail("Can't get bot text reply %u! No bots texts replies!", replyType);
        return "";
    }

    std::vector<BotTextEntry>& list = botTexts["reply"];
    std::vector<BotTextEntry> proper_list;
    for (auto text : list)
    {
        if (text.m_replyType == replyType)
            proper_list.push_back(text);
    }

    BotTextEntry textEntry = proper_list[urand(0, proper_list.size() - 1)];
    std::string botText;
    int32 localePrio = GetLocalePriority();
    if (localePrio == -1)
        botText = textEntry.m_text;
    else
    {
        if (!textEntry.m_text_locales[localePrio].empty())
            botText = textEntry.m_text_locales[localePrio];
        else
            botText = textEntry.m_text;
    }

    ReplacePlaceholders(botText, placeholders);

    return botText;
}

std::string PlayerbotTextMgr::GetBotText(ChatReplyType replyType, std::string name)
{
    std::map<std::string, std::string> placeholders;
    placeholders["%s"] = name;

    return GetBotText(replyType, placeholders);
}

// probabilities

bool PlayerbotTextMgr::rollTextChance(std::string name)
{
    if (!botTextChance[name])
        return true;

    return urand(0, 100) < botTextChance[name];
}

bool PlayerbotTextMgr::GetBotText(std::string name, std::string &text)
{
    if (!rollTextChance(name))
        return false;

    text = GetBotText(name);
    return !text.empty();
}

bool PlayerbotTextMgr::GetBotText(std::string name, std::string& text, std::map<std::string, std::string> placeholders)
{
    if (!rollTextChance(name))
        return false;

    text = GetBotText(name, placeholders);
    return !text.empty();
}

void PlayerbotTextMgr::AddLocalePriority(int32 locale)
{
    if (locale == -1)
        return;

    botTextLocalePriority[locale]++;
}

int32 PlayerbotTextMgr::GetLocalePriority()
{
    int32 topLocale = -1;
    uint32 tempCheck = 0;

    // if no real players online, reset top locale
    if (!sWorld.GetActiveSessionCount())
    {
        ResetLocalePriority();
        return 0;
    }

    for (uint8 i = 1; i < MAX_LOCALE; ++i)
    {
        int32 locale_idx = sObjectMgr.GetStorageLocaleIndexFor(LocaleConstant(i));
        if (locale_idx == -1)
            continue;

        if (botTextLocalePriority[locale_idx] > tempCheck)
        {
            tempCheck = botTextLocalePriority[locale_idx];
            topLocale = locale_idx;
        }
    }

    return topLocale;
}

void PlayerbotTextMgr::ResetLocalePriority()
{
    for (uint8 i = 1; i < MAX_LOCALE; ++i)
    {
        botTextLocalePriority[sObjectMgr.GetStorageLocaleIndexFor(LocaleConstant(i))] = 0;
    }
}