//
// ids_reverse_translator.cc — ids_reverse 统一检字翻译器（C++，IDS4
// 数据格式，面向查询延迟）。
//
#include "ids_reverse_translator.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/translation.h>

#include "ids_reverse_config.h"
#include "ids_reverse_input.h"
#include "ids_reverse_search.h"
#include "ids_store.h"

namespace ids_reverse {

namespace {

using rime::hash_set;

// ---------------------------------------------------------------------------
// UTF-8 帮助函数
// ---------------------------------------------------------------------------

uint32_t DecodeNext(const std::string& s, size_t* i) {
  size_t n = s.size();
  if (*i >= n)
    return 0;
  unsigned char b1 = static_cast<unsigned char>(s[*i]);
  if (b1 < 0x80) {
    *i += 1;
    return b1;
  }
  if (b1 < 0xE0) {
    if (*i + 1 >= n)
      return 0;
    uint32_t b2 = static_cast<unsigned char>(s[*i + 1]);
    *i += 2;
    return (b1 - 0xC0) * 0x40 + (b2 - 0x80);
  }
  if (b1 < 0xF0) {
    if (*i + 2 >= n)
      return 0;
    uint32_t b2 = static_cast<unsigned char>(s[*i + 1]);
    uint32_t b3 = static_cast<unsigned char>(s[*i + 2]);
    *i += 3;
    return (b1 - 0xE0) * 0x1000 + (b2 - 0x80) * 0x40 + (b3 - 0x80);
  }
  if (*i + 3 >= n)
    return 0;
  uint32_t b2 = static_cast<unsigned char>(s[*i + 1]);
  uint32_t b3 = static_cast<unsigned char>(s[*i + 2]);
  uint32_t b4 = static_cast<unsigned char>(s[*i + 3]);
  *i += 4;
  return (b1 - 0xF0) * 0x40000 + (b2 - 0x80) * 0x1000 + (b3 - 0x80) * 0x40 +
         (b4 - 0x80);
}

uint32_t CpOf(const std::string& ch) {
  size_t i = 0;
  return DecodeNext(ch, &i);
}

std::string CpToUtf8(uint32_t cp) {
  char buf[4];
  size_t n;
  if (cp < 0x80) {
    buf[0] = static_cast<char>(cp);
    n = 1;
  } else if (cp < 0x800) {
    buf[0] = static_cast<char>(0xC0 | (cp >> 6));
    buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    buf[0] = static_cast<char>(0xE0 | (cp >> 12));
    buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    buf[0] = static_cast<char>(0xF0 | (cp >> 18));
    buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 4;
  }
  return std::string(buf, n);
}

std::vector<std::string> Utf8Chars(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0, n = s.size();
  while (i < n) {
    size_t old = i;
    DecodeNext(s, &i);
    if (i == old) {
      out.push_back(s.substr(old, 1));
      i = old + 1;
    } else {
      out.push_back(s.substr(old, i - old));
    }
  }
  return out;
}

bool IsAsciiAlphaOnly(const std::string& s) {
  for (unsigned char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
      return false;
  }
  return !s.empty();
}

std::string Join(const std::vector<std::string>& v, const std::string& sep) {
  std::string r;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i)
      r += sep;
    r += v[i];
  }
  return r;
}

std::string FormatIds(const std::string& raw_ids, const std::string& format) {
  if (format == "unicode" || format.empty()) {
    return raw_ids;
  }
  std::string out;
  size_t i = 0;
  size_t n = raw_ids.size();
  while (i < n) {
    size_t old = i;
    uint32_t cp = DecodeNext(raw_ids, &i);
    if (cp >= 0x2FF0 && cp <= 0x2FFF) {
      if (format == "bracket") {
        static const char* kIdcLabels[] = {
            "[左右]",   "[上下]",   "[左中右]",   "[上中下]",
            "[全包]",   "[上包]",   "[下包]",     "[左包]",
            "[左上包]", "[右上包]", "[左下包]",   "[交叉]",
            "[右包]",   "[右下包]", "[水平翻转]", "[旋转]"};
        out += kIdcLabels[cp - 0x2FF0];
      } else if (format == "plain") {
        // 直接省略 IDC 结构字符
      } else {
        out += raw_ids.substr(old, i - old);
      }
    } else {
      out += raw_ids.substr(old, i - old);
    }
  }
  return out;
}

// O 模式当前 pending 段的多部件解析结果。
// tokens 为拼音/输入码音节或单个汉字；开头的 - / + 会被跳过。
struct PendingComponents {
  bool valid = false;
  std::vector<std::string> tokens;
};

bool IsPendingSeparator(unsigned char c) {
  return c == '\'' || c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
         c == '.' || c == '_';
}

// 与 ids_text.cc 的 IsComponentChar 保持一致；此处避免包含 ids_text.h
// 造成与匿名命名空间内同名帮助函数的歧义。
bool IsComponentCodePoint(uint32_t cp) {
  if (cp >= 0x2FF0 && cp <= 0x2FFF)
    return false;
  return (cp >= 0x2E80 && cp <= 0x2FDF) || (cp >= 0x31C0 && cp <= 0x31EF) ||
         cp == 0x3007 || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x2FA1F) || (cp >= 0x30000 && cp <= 0x323AF);
}

// 完整部件表达式，例如 木木口! / 森木-；按逆波兰后缀解释。
// 不再接受括号；ASCII 拼音和空白也不属于表达式。
bool IsComponentExpression(const std::string& text) {
  if (text.empty())
    return false;
  size_t count = 0;
  bool has_component = false;
  size_t i = 0;
  while (i < text.size()) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      return false;
    if (c == '+' || c == '-' || c == '!') {
      ++i;
      ++count;
      if (count > 32)
        return false;
      continue;
    }
    if (c < 0x80)
      return false;
    size_t start = i;
    uint32_t cp = DecodeNext(text, &i);
    if (i == start || cp == 0 || !IsComponentCodePoint(cp))
      return false;
    has_component = true;
    ++count;
    if (count > 32)
      return false;
  }
  return has_component;
}

PendingComponents ParsePendingComponents(IdsStore& store,
                                         const std::string& pending,
                                         const InputCodec& codec) {
  PendingComponents out;
  if (pending.empty())
    return out;

  size_t i = 0;

  std::string ascii;
  auto flush_ascii = [&]() -> bool {
    if (ascii.empty())
      return true;
    std::vector<std::string> segs = codec.Segment(ascii);
    ascii.clear();
    for (const auto& seg : segs) {
      if (!seg.empty())
        out.tokens.push_back(seg);
    }
    return !segs.empty();
  };

  while (i < pending.size()) {
    unsigned char c = static_cast<unsigned char>(pending[i]);
    if (c == '-' || c == '+' || c == '!' || c == '(' || c == ')')
      return out;  // 一次连写内部不支持运算符或括号
    if (IsPendingSeparator(c)) {
      if (!flush_ascii())
        return out;
      ++i;
      continue;
    }
    if (c < 0x80) {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        return out;
      ascii.push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    if (!flush_ascii())
      return out;
    size_t start = i;
    uint32_t cp = DecodeNext(pending, &i);
    if (i == start || cp == 0 || !IsComponentCodePoint(cp))
      return out;
    out.tokens.push_back(pending.substr(start, i - start));
  }
  if (!flush_ascii())
    return out;
  // 限制一次多部件输入的长度，避免组合候选爆炸。
  if (out.tokens.size() > 8)
    return out;
  out.valid = !out.tokens.empty();
  return out;
}

const char* kBlockNames[17] = {
    "基本",     "扩A",      "扩B",      "扩C",  "扩D", "扩E",
    "扩F",      "扩G",      "扩H",      "扩I",  "扩J", "兼容区",
    "兼容补充", "部首补充", "康熙部首", "笔画", "CJK"};

std::string BuildComment(IdsStore& store,
                         uint32_t id,
                         bool show_pinyin,
                         bool show_ids,
                         bool show_unicode,
                         bool show_block,
                         const std::string& ids_format) {
  const uint8_t* py = nullptr;
  const uint8_t* ids = nullptr;
  uint16_t py_len = 0;
  uint16_t ids_len = 0;
  store.Cold(id, &py, &py_len, &ids, &ids_len);
  const HotRec& hot = store.Hot(id);
  std::vector<std::string> parts;
  if (show_pinyin && py_len > 0) {
    parts.push_back(std::string(reinterpret_cast<const char*>(py), py_len));
  }
  if (show_ids && ids_len > 0) {
    parts.push_back(FormatIds(
        std::string(reinterpret_cast<const char*>(ids), ids_len), ids_format));
  }
  uint32_t cp = store.CpOfId(id);
  if (show_unicode) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04X", cp);
    parts.push_back(buf);
  }
  if (show_block) {
    const char* bname = hot.block < 17 ? kBlockNames[hot.block] : "基本";
    parts.push_back(bname);
  }
  return Join(parts, " ");
}

}  // namespace

// ---------------------------------------------------------------------------
// IdsReverseTranslator
// ---------------------------------------------------------------------------

IdsReverseTranslator::IdsReverseTranslator(const rime::Ticket& ticket)
    : rime::Translator(ticket) {
  LoadReverseConfig(ticket, &name_space_, &config_);
}

const InputCodec& IdsReverseTranslator::Codec() {
  if (!codec_)
    codec_ = InputCodec::Get(IdsStore::Instance(), config_);
  return *codec_;
}

bool IdsReverseTranslator::EnsureLoaded() {
  if (!EnsureIdsStoreLoaded(config_))
    return false;
  MergeCustomDisabledBlocks(&config_.disabled_blocks,
                            &config_.disabled_blocks_enabled);
  return true;
}

void IdsReverseTranslator::BuildResultCandidates(
    IdsStore& store,
    const std::vector<uint32_t>& results,
    const rime::Segment& segment,
    rime::FifoTranslation* translation) {
  bool show_pinyin = config_.show_pinyin;
  bool show_ids = config_.show_ids;
  bool show_unicode = config_.show_unicode;
  bool show_block = config_.show_block;
  rime::Context* ctx = engine_ ? engine_->context() : nullptr;
  if (ctx) {
    if (config_.has_sw_pinyin)
      show_pinyin = ctx->get_option("show_pinyin");
    if (config_.has_sw_ids)
      show_ids = ctx->get_option("show_ids");
    if (config_.has_sw_unicode)
      show_unicode = ctx->get_option("show_unicode");
    if (config_.has_sw_block)
      show_block = ctx->get_option("show_block");
  }
  std::string preedit = "\t";

  for (uint32_t id : results) {
    uint32_t cp = store.CpOfId(id);
    std::string text = CpToUtf8(cp);
    std::string comment =
        BuildComment(store, id, show_pinyin, show_ids, show_unicode, show_block,
                     config_.ids_format);
    auto cand = rime::New<rime::SimpleCandidate>(
        "ids_reverse", segment.start, segment.end, text, comment, preedit);
    cand->set_quality(100.0);
    translation->Append(cand);
  }
}

void IdsReverseTranslator::BuildComponentCandidates(
    IdsStore& store,
    const std::vector<uint32_t>& results,
    const rime::Segment& segment,
    rime::FifoTranslation* translation) {
  for (uint32_t id : results) {
    if (config_.disabled_blocks_enabled &&
        config_.disabled_blocks.count(static_cast<int>(store.Hot(id).block))) {
      continue;
    }
    uint32_t cp = store.CpOfId(id);
    std::string text = CpToUtf8(cp);
    auto cand = rime::New<rime::SimpleCandidate>(
        "hanzi_component", segment.start, segment.end, text, "");
    cand->set_quality(100.0);
    translation->Append(cand);
  }
}

void IdsReverseTranslator::BuildMultiComponentCandidates(
    IdsStore& store,
    const std::vector<std::string>& tokens,
    const rime::Segment& segment,
    rime::FifoTranslation* translation) {
  constexpr size_t kPerTokenLimit = 4;
  constexpr size_t kMaxCandidates = 40;
  const hash_set<int>* disabled =
      config_.disabled_blocks_enabled ? &config_.disabled_blocks : nullptr;

  struct Option {
    std::string text;
    int rank = 0;
    uint32_t freq = 0;
  };

  std::vector<std::vector<Option>> options;
  options.reserve(tokens.size());
  for (const auto& token : tokens) {
    std::vector<Option> opts;
    if (!IsAsciiAlphaOnly(token) && Utf8Chars(token).size() == 1) {
      // 输入串里直接给出的汉字部件。
      uint32_t cp = CpOf(token);
      uint32_t id = cp ? store.IdOfCp(cp) : kInvalidId;
      if (id == kInvalidId)
        return;
      if (disabled && disabled->count(static_cast<int>(store.Hot(id).block)))
        return;
      opts.push_back({token, 0, store.Hot(id).freq});
    } else if (store.valid_pinyins().count(token)) {
      // Segment 解码出的标准拼音（不再当作输入码处理）。
      std::vector<uint32_t> ids = GetPinyinCandidates(store, token, 80);
      for (uint32_t id : ids) {
        if (disabled && disabled->count(static_cast<int>(store.Hot(id).block)))
          continue;
        uint32_t cp = store.CpOfId(id);
        if (!cp)
          continue;
        opts.push_back(
            {CpToUtf8(cp), static_cast<int>(opts.size()), store.Hot(id).freq});
        if (opts.size() >= kPerTokenLimit)
          break;
      }
    } else {
      // 原始输入码片段：按编码前缀展开候选。
      std::vector<uint32_t> ids =
          GetComponentCandidates(store, token, Codec(), 80);
      for (uint32_t id : ids) {
        if (disabled && disabled->count(static_cast<int>(store.Hot(id).block)))
          continue;
        uint32_t cp = store.CpOfId(id);
        if (!cp)
          continue;
        opts.push_back(
            {CpToUtf8(cp), static_cast<int>(opts.size()), store.Hot(id).freq});
        if (opts.size() >= kPerTokenLimit)
          break;
      }
    }
    if (opts.empty())
      return;
    options.push_back(std::move(opts));
  }

  struct Combo {
    std::string text;
    int rank_sum = 0;
    uint64_t freq_sum = 0;
  };
  auto better = [](const Combo& a, const Combo& b) {
    if (a.rank_sum != b.rank_sum)
      return a.rank_sum < b.rank_sum;
    if (a.freq_sum != b.freq_sum)
      return a.freq_sum > b.freq_sum;
    return a.text < b.text;
  };

  std::vector<Combo> combos(1);
  for (const auto& opts : options) {
    std::vector<Combo> next;
    next.reserve(combos.size() * opts.size());
    for (const auto& base : combos) {
      for (const auto& opt : opts) {
        next.push_back({base.text + opt.text, base.rank_sum + opt.rank,
                        base.freq_sum + opt.freq});
      }
    }
    std::sort(next.begin(), next.end(), better);
    if (next.size() > kMaxCandidates * 4)
      next.resize(kMaxCandidates * 4);
    combos.swap(next);
  }
  std::sort(combos.begin(), combos.end(), better);

  size_t limit = kMaxCandidates;
  if (config_.max_results > 0)
    limit = std::min(limit, static_cast<size_t>(config_.max_results));

  hash_set<std::string> seen;
  size_t emitted = 0;
  for (const auto& combo : combos) {
    if (emitted >= limit)
      break;
    if (combo.text.empty() || !seen.insert(combo.text).second)
      continue;
    auto cand = rime::New<rime::SimpleCandidate>(
        "hanzi_component", segment.start, segment.end, combo.text, "多部件");
    cand->set_quality(100.0);
    translation->Append(cand);
    ++emitted;
  }
}

void IdsReverseTranslator::BuildVariantCandidates(
    IdsStore& store,
    uint32_t base_id,
    const std::string& base,
    const std::string& preedit,
    const rime::Segment& segment,
    rime::FifoTranslation* translation) {
  bool show_pinyin = config_.show_pinyin;
  bool show_ids = config_.show_ids;
  bool show_unicode = config_.show_unicode;
  bool show_block = config_.show_block;
  rime::Context* ctx = engine_ ? engine_->context() : nullptr;
  if (ctx) {
    if (config_.has_sw_pinyin)
      show_pinyin = ctx->get_option("show_pinyin");
    if (config_.has_sw_ids)
      show_ids = ctx->get_option("show_ids");
    if (config_.has_sw_unicode)
      show_unicode = ctx->get_option("show_unicode");
    if (config_.has_sw_block)
      show_block = ctx->get_option("show_block");
  }

  const uint32_t* variants = nullptr;
  uint16_t n = 0;
  if (base_id != kInvalidId)
    store.Variants(base_id, &variants, &n);

  if (n == 0) {
    // 无异体字：提示候选，text 为基准字，选中即上屏。
    auto cand = rime::New<rime::SimpleCandidate>("ids_reverse", segment.start,
                                                 segment.end, base,
                                                 "〔无异体字〕", preedit);
    cand->set_quality(100.0);
    translation->Append(cand);
    return;
  }

  for (uint16_t i = 0; i < n; ++i) {
    uint32_t vid = variants[i];
    if (vid >= store.num_chars())
      continue;
    std::string text = CpToUtf8(store.CpOfId(vid));
    std::string comment =
        BuildComment(store, vid, show_pinyin, show_ids, show_unicode,
                     show_block, config_.ids_format);
    auto cand = rime::New<rime::SimpleCandidate>(
        "ids_reverse", segment.start, segment.end, text, comment, preedit);
    cand->set_quality(100.0);
    translation->Append(cand);
  }
}

rime::an<rime::Translation> IdsReverseTranslator::QueryHybrid(
    const rime::Segment& segment) {
  rime::Context* ctx = engine_ ? engine_->context() : nullptr;
  if (!ctx || !IsReverseActive(ctx, config_.prefix))
    return nullptr;
  bool has_mode = segment.HasTag(config_.tag);
  bool has_component = segment.HasTag("hanzi_component");
  if (!has_mode && !has_component)
    return nullptr;
  if (!EnsureLoaded())
    return nullptr;

  IdsStore& store = IdsStore::Instance();
  HybridState st = ReadHybridState(ctx, config_.prefix);
  auto translation = rime::New<rime::FifoTranslation>();
  const std::string& pending = st.pending;
  bool variant_pending = EndsWith(pending, config_.variant_trigger);

  const std::string variant_result = ctx->get_property(kVariantsResultProperty);
  bool variant_result_stage = false;
  if (!variant_pending && !variant_result.empty()) {
    if (EndsWith(pending, variant_result)) {
      // 结果替换：raw 表达式以异体字结尾。
      variant_result_stage = true;
    } else if (pending.empty() && !st.components.empty()) {
      // 待输入原字替换：异体字已作为最后一个部件加入状态。
      variant_result_stage = (st.components.back() == variant_result);
    }
  }
  if (variant_result_stage) {
    // 结果替换后的异体字作为当前结果候选，而不是把
    // “前置部件 + 异体字”重新解析成多部件查询。
    uint32_t cp = CpOf(variant_result);
    uint32_t id = cp ? store.IdOfCp(cp) : kInvalidId;
    std::string comment;
    if (id != kInvalidId) {
      bool show_pinyin = config_.show_pinyin;
      bool show_ids = config_.show_ids;
      bool show_unicode = config_.show_unicode;
      bool show_block = config_.show_block;
      if (config_.has_sw_pinyin)
        show_pinyin = ctx->get_option("show_pinyin");
      if (config_.has_sw_ids)
        show_ids = ctx->get_option("show_ids");
      if (config_.has_sw_unicode)
        show_unicode = ctx->get_option("show_unicode");
      if (config_.has_sw_block)
        show_block = ctx->get_option("show_block");
      comment = BuildComment(store, id, show_pinyin, show_ids, show_unicode,
                             show_block, config_.ids_format);
    }
    // 部件形式的状态末尾带有分隔引号，用空 preedit 隐藏它；
    // raw 表达式形式则保留原始输入作为 preedit。
    std::string preedit = pending.empty() ? "\t" : std::string();
    auto cand = rime::New<rime::SimpleCandidate>("ids_reverse", segment.start,
                                                 segment.end, variant_result,
                                                 comment, preedit);
    cand->set_quality(100.0);
    translation->Append(cand);
    return translation;
  }

  if (variant_pending) {
    // 异体字查询：基准字由 processor 写入 property。
    // 允许 O葵/ 这种尚未加入正向部件的单字候选查询。
    std::string base = ctx->get_property(kVariantsProperty);
    if (!base.empty()) {
      uint32_t cp = CpOf(base);
      uint32_t base_id = cp ? store.IdOfCp(cp) : kInvalidId;
      // 触发符前仍有输入时，基准字会被候选 preedit 替换，需要一并显示；
      // 若基准字已由前一段显示（O艹癸 → /），则只显示触发符。
      const std::string query =
          pending.substr(0, pending.size() - config_.variant_trigger.size());
      std::string preedit = config_.variant_trigger + "\t";
      if (!query.empty())
        preedit = base + config_.variant_trigger + "\t";
      BuildVariantCandidates(store, base_id, base, preedit, segment,
                             translation.get());
    }
  } else if (pending.empty() && !st.components.empty()) {
    std::vector<uint32_t> results = SearchHybrid(
        store, st.components, Codec(), config_.max_results,
        config_.disabled_blocks_enabled ? &config_.disabled_blocks : nullptr);
    BuildResultCandidates(store, results, segment, translation.get());
  } else if (!pending.empty() && has_component) {
    PendingComponents parsed = ParsePendingComponents(store, pending, Codec());
    if (parsed.valid && parsed.tokens.size() >= 2) {
      // 多音节/多汉字连写：一次给出可加入多个部件的组合候选。
      BuildMultiComponentCandidates(store, parsed.tokens, segment,
                                    translation.get());
    } else if (parsed.valid && parsed.tokens.size() == 1) {
      if (!IsAsciiAlphaOnly(parsed.tokens[0])) {
        // pending 中已有单个汉字。
        auto cand =
            rime::New<rime::SimpleCandidate>("hanzi_component", segment.start,
                                             segment.end, parsed.tokens[0], "");
        cand->set_quality(100.0);
        translation->Append(cand);
      } else {
        // 未解码的输入码仍走正常部件选字流程。
        std::vector<uint32_t> results =
            GetComponentCandidates(store, pending, Codec(), 80);
        BuildComponentCandidates(store, results, segment, translation.get());
      }
    } else if (IsComponentExpression(pending)) {
      // 原始汉字表达式：一次加入整个表达式，例如 木木口!。
      auto cand = rime::New<rime::SimpleCandidate>(
          "hanzi_component", segment.start, segment.end, pending, "部件表达式");
      cand->set_quality(100.0);
      translation->Append(cand);
    } else {
      std::vector<uint32_t> results =
          GetComponentCandidates(store, pending, Codec(), 80);
      BuildComponentCandidates(store, results, segment, translation.get());
    }
  }

  if (translation->size() == 0)
    return nullptr;
  return translation;
}

rime::an<rime::Translation> IdsReverseTranslator::Query(
    const std::string& input,
    const rime::Segment& segment) {
  (void)input;
  return QueryHybrid(segment);
}

}  // namespace ids_reverse
