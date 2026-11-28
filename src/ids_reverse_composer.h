//
// ids_reverse_composer.h — ids_reverse 原生 processor / segmentor / filter。
//
#ifndef RIME_IDS_REVERSE_IDS_REVERSE_COMPOSER_H_
#define RIME_IDS_REVERSE_IDS_REVERSE_COMPOSER_H_

#include <string>

#include <rime/common.h>
#include <rime/context.h>
#include <rime/filter.h>
#include <rime/key_event.h>
#include <rime/processor.h>
#include <rime/segmentor.h>
#include <rime/translation.h>

#include "ids_reverse_config.h"

namespace ids_reverse {

// 处理 O 模式（部件/拼音/输入码混合检字）的按键。
class IdsReverseProcessor : public rime::Processor {
 public:
  explicit IdsReverseProcessor(const rime::Ticket& ticket);
  ~IdsReverseProcessor() override;

  rime::ProcessResult ProcessKeyEvent(const rime::KeyEvent& key_event) override;

 private:
  void OnSelectBefore(rime::Context* ctx);
  void OnSelectAfter(rime::Context* ctx);
  void OnUpdate(rime::Context* ctx);
  bool HandleIdsHotkey(const rime::KeyEvent& key_event, rime::Context* ctx);
  bool HandleSelectionKey(int digit, rime::Context* ctx);
  bool MatchesVariantReplaceKey(const rime::KeyEvent& key_event) const;
  bool TryReplaceVariant(rime::Context* ctx);

  ReverseConfig config_;
  rime::connection select_before_;
  rime::connection select_after_;
  rime::connection update_connection_;

  bool has_selection_ = false;
  bool selected_is_result_ = false;
  std::string selected_text_;
  std::string selected_component_;
  std::string selected_tail_;
  bool saved_auto_commit_ = false;
  rime::KeyEvent variant_replace_key_;
  bool has_variant_replace_key_ = false;
};

// O 模式动态分段。
class IdsReverseSegmentor : public rime::Segmentor {
 public:
  explicit IdsReverseSegmentor(const rime::Ticket& ticket);
  bool Proceed(rime::Segmentation* segmentation) override;

 private:
  ReverseConfig config_;
};

// O 模式做单字过滤与去重。
class IdsReverseFilter : public rime::Filter {
 public:
  explicit IdsReverseFilter(const rime::Ticket& ticket);

  rime::an<rime::Translation> Apply(rime::an<rime::Translation> translation,
                                    rime::CandidateList* candidates) override;

 private:
  ReverseConfig config_;
};

}  // namespace ids_reverse

#endif  // RIME_IDS_REVERSE_IDS_REVERSE_COMPOSER_H_
