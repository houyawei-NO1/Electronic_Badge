/**
 * @file font_chars.c
 * @brief Runtime Chinese font support checking
 *
 * Auto-generated from lv_font_simsun_16_cjk.c character set.
 * Total Chinese chars: 1097
 */
#include "font_chars.h"

/* All CJK Unified Ideographs supported by lv_font_simsun_16_cjk.
 * Extracted from the font source file on 2026-06-10.
 * To regenerate: scripts/extract_font_chars.py */
static const char *const supported_cjk =
    "一七万丈三上下不专且世両並中主久之乎乏乗九也了予争事二五些亡交亦京人什今介仍仕"
    "他付代令以仮件任份企伊休会伝伸似但位低住体何余作你使來例供依価便係保信修個們候"
    "借値值做停健側備傳僅働像僕價億優元兄充先光克免兒党入內全兩八公六共其具内円冊再"
    "写冬冷凍凝処出分切刊列初判別利到制券刻則削前剛割創劃力功加助努勉動務勝勤勵包化"
    "北医區十千午半卒協南単危即卻厚原厳去参參又及友反取受口古句另只叫可台史右号司吃"
    "各合同名向否吧呀告呢周味呼命和咲品員哪商啊問啦善喉喜喝單営嗎嗯嘛嚴四回因困図国"
    "國圍園團土在地均坊坐型域執基報場塊塩境増增壊壓士声売変夏夕外多夜夠大天太夫央失"
    "奇契套女她好如妳妹妻姉始姐委娘婚婦媒媽嫌嬉子字存季学孩孫學它宅宇守安完官宙定实"
    "実客室害家容宿寄密富寒寝察實寫寺対射將專對導小少尚尤就尺局居届屋展履屬山島嵌川"
    "州工左差己已市布希師席帯帰帳帶常幅平年幸幾広底店府度座庫庭康廠建式引弟弱張強当"
    "形影役彼往待很律後徒従得從御復心必忘忙快念怎怒怕思急性息您悪悲情想愈意愛感態慣"
    "慧慮應懸成我或戦戰戻房所手才打払找技把投押担拉招拡括拭拿持指挙捕捨授排掛採探"
    "接控推描提換揮揺携擁擇擔據支改放政故效敗教敢数整文料断新斷方於施旁旅族既日早时"
    "昇明易昔星映春昨是昼時晩普景晴智暇暑暖暗曇曜曲更書曾替最會月有朋服望朝期木未末"
    "本札机材村束条来杯東析林果某查柱査柿校株根格桃案條械森検楚業極楽概構様樂標模樣"
    "横樹橋機次欲歌歐歡止正此步歩歯歲歳歷死殊残段母毎每比毛氏民气気水氷永求汚池決沈"
    "沒油治況泊法波泣注泳洋洗洲活派流浅浴海消涼深混清渇済渉減渡温港湖源準溝滿漢漸濃"
    "濟灣火災点為無然煙熱營爭父爸片牛牠物特犬犯状狀独狭猫猿獲玩現球理環甘甚生產産用"
    "田由申电男町画界留番畫異當疲病痛発發白百的皆皿盗目直相看真眠眾着知短石砂研破確"
    "示礼社祖祝神祭禁秀私秋种科秘移程種積究空窓立站童笑符第筆等答策算管節範簡米精糖"
    "系紀約紙素細紹終組経結絡給統絵經続維網緑緒線締緩練總績織繰繼續统缺置美義習老"
    "考者而耳聞聯聲職聽肉肩肯育背胸能脱腕腦腰膝臉自至臺與興舉航般船良色花若苦英茶荷"
    "菓菜落葉著蔵薄薬藝蘇處行術表被裏補裡製複西要見規視覚親覺觀角解触言計訊討訓記訪"
    "設許訳訴評試話該詳誌認誕誘語說説読誰課調談請論諸謂講謝識議護變讓计豐象負財貧"
    "販責買貸費貿賃資質賽赤走起超越趣足跟路身車軟転軽較載輕輪輸辛辞辦農辺込迎近返迫"
    "追退送逃透逐這通速造連逮週進遅遊運過道達違遠適選避還邊那邪部郵都配酒酔酸醫重野"
    "量金針鉄鉛銀錢錯録長閉開間関關附降限院除陸陽階際障隣隨隻集雑雖雙離難雨雪雲電需"
    "震青静非面革靴音響頃項須頗領頭頼題顔願類顯風飛食飯飲館首香駄駅騒験驗體高髪魚鳥"
    "麗麼黄黒點黨鼓鼻";

bool font_supports_chinese(const char *str)
{
    if (!str || str[0] == '\0') return true;

    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        /* UTF-8 decode: CJK chars are 3-byte sequences E4~E9 xx xx */
        if (*p >= 0xE4 && *p <= 0xE9) {
            /* 3-byte UTF-8: decode to Unicode codepoint */
            uint32_t cp = ((uint32_t)(p[0] & 0x0F) << 12)
                        | ((uint32_t)(p[1] & 0x3F) << 6)
                        | ((uint32_t)(p[2] & 0x3F));
            /* Only check CJK Unified Ideographs (U+4E00~U+9FFF) */
            if (cp >= 0x4E00 && cp <= 0x9FFF) {
                /* Convert back to UTF-8 bytes for string search */
                char utf8[4] = {
                    (char)(0xE0 | ((cp >> 12) & 0x0F)),
                    (char)(0x80 | ((cp >> 6) & 0x3F)),
                    (char)(0x80 | (cp & 0x3F)),
                    '\0'
                };
                if (strstr(supported_cjk, utf8) == NULL) {
                    return false;  /* Character not in font */
                }
            }
            p += 3;
        } else if (*p < 0x80) {
            /* ASCII - always supported */
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            /* 2-byte UTF-8 - skip */
            p += 2;
        } else if ((*p & 0xF0) == 0xF0) {
            /* 4-byte UTF-8 - skip */
            p += 4;
        } else {
            p++;
        }
    }
    return true;
}
