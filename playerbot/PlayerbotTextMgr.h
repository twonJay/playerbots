#ifndef _PlayerbotTextMgr_H
#define _PlayerbotTextMgr_H

#include "Common.h"
#include "PlayerbotAIBase.h"

#define BOT_TEXT(name) sPlayerbotTextMgr.GetBotText(name)
#define BOT_TEXT2(name, replace) sPlayerbotTextMgr.GetBotText(name, replace)
// picks with the bot's zone, level and faction taken into account (see my_bot_texts)
#define BOT_TEXT3(name, replace, bot) sPlayerbotTextMgr.GetBotText(name, replace, bot)

struct BotTextEntry
{
    BotTextEntry(std::string name, std::string text, std::map<int32, std::string> text_locales, uint32 say_type, uint32 reply_type) : m_name(name), m_text(text), m_text_locales(text_locales), m_sayType(say_type), m_replyType(reply_type) {}
    std::string m_name;
    std::string m_text;
    std::map<int32, std::string> m_text_locales;
    uint32 m_sayType;
    uint32 m_replyType;

    // Restrictions, only set for rows from the local my_bot_texts table. A line
    // with none of them can be said anywhere by anyone.
    std::set<uint32> m_zones;               // zone ids the bot must be in
    uint32 m_minLevel = 0;                  // 0 = no bound
    uint32 m_maxLevel = 0;
    uint32 m_team = 0;                      // 0 = any, otherwise ALLIANCE / HORDE

    bool IsZoned() const { return !m_zones.empty(); }
    bool IsRestricted() const { return IsZoned() || m_minLevel || m_maxLevel || m_team; }
    bool Matches(uint32 zoneId, uint32 level, uint32 team) const
    {
        if (IsZoned() && m_zones.find(zoneId) == m_zones.end())
            return false;
        if (m_minLevel && level < m_minLevel)
            return false;
        if (m_maxLevel && level > m_maxLevel)
            return false;
        return !m_team || m_team == team;
    }
};

struct ChatReplyData
{
    ChatReplyData(uint32 guid, uint32 type, std::string chat) : m_guid(guid), m_type(type), m_chat(chat) {}
    uint32 m_type, m_guid = 0;
    std::string m_chat = "";
};

struct ChatQueuedReply
{
    ChatQueuedReply() : m_type(0), m_guid1(0), m_guid2(0), m_msg(), m_chanName(), m_name(), m_time(0) {}
    ChatQueuedReply(uint32 type, uint32 guid1, uint32 guid2, std::string msg, std::string chanName, std::string name, time_t time) : m_type(type), m_guid1(guid1), m_guid2(guid2), m_msg(msg), m_chanName(chanName), m_name(name), m_time(time) {}
    uint32 m_type;
    uint32 m_guid1;
    uint32 m_guid2;
    std::string m_msg;
    std::string m_chanName;
    std::string m_name;
    time_t m_time;
};

enum ChatReplyType
{
    REPLY_NOT_UNDERSTAND,
    REPLY_GRUDGE,
    REPLY_VICTIM,
    REPLY_ATTACKER,
    REPLY_HELLO,
    REPLY_NAME,
    REPLY_ADMIN_ABUSE
};

class PlayerbotTextMgr
{
    public:
        PlayerbotTextMgr();
        virtual ~PlayerbotTextMgr();
        static PlayerbotTextMgr& instance()
        {
            static PlayerbotTextMgr instance;
            return instance;
        }

	public:
        static void ReplaceAll(std::string& str, const std::string& from, const std::string& to);
        static void ReplacePlaceholders(std::string& text, const std::map<std::string, std::string>& placeholders);
        static std::string GetReplacePlaceholders(const std::string& text, const std::map<std::string, std::string>& placeholders) { std::string retText = text; ReplacePlaceholders (retText, placeholders); return retText;  }

        std::string GetBotText(std::string name, std::map<std::string, std::string> placeholders);
        std::string GetBotText(std::string name);
        std::string GetBotText(std::string name, std::map<std::string, std::string> placeholders, Player* bot);
        std::string GetBotText(ChatReplyType replyType, std::map<std::string, std::string> placeholders);
        std::string GetBotText(ChatReplyType replyType, std::string name);
        bool GetBotText(std::string name, std::string& text);
        bool GetBotText(std::string name, std::string& text, std::map<std::string, std::string> placeholders);
        void LoadBotTexts();
        void LoadBotTextChance();

        bool rollTextChance(std::string text);

        int32 GetLocalePriority();
        void AddLocalePriority(int32 locale);
        void ResetLocalePriority();

    private:
        std::map<std::string, std::vector<BotTextEntry>> botTexts;
        std::map<std::string, uint32> botTextChance;
        uint32 botTextLocalePriority[MAX_LOCALE];
};

#define sPlayerbotTextMgr PlayerbotTextMgr::instance()

#endif
