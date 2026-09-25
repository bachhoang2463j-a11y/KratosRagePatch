#pragma once
// ===========================================================================
// KratosRagePatch 离线 bridge 集成测试 —— 受控伪游戏 API（mock harness）。
//
// 定位与边界（先读这段）：
//  * 这里只实现 src/GameBridge.cpp 真正调用的 RE:: / SKSE:: 成员，是 **mock**，
//    不是引擎/游戏模拟；不实现任何 Creation Engine 真实语义。
//  * 绝不复制、绝不重写 GameBridge 的任何函数体或规则：所有断言必须由真实
//    GameBridge.cpp 的行为产生，fake 只负责记录副作用（施法、事件、spell marker、
//    active effect、任务队列）。
//  * 形状必须与 AE-only（ENABLE_SKYRIM_AE ON / SE OFF）构建一致，**不得存在真实 API
//    没有、或该配置下取不到的成员**，否则会掩盖真实编译问题。已按此对齐的关键点：
//      - Actor 没有 GetActiveEffectList()（真实里它被 #ifndef ENABLE_SKYRIM_AE 排除），
//        必须走 actor->AsMagicTarget()->GetActiveEffectList()；
//      - MagicTarget::GetActiveEffectList() 非 const，返回 BSSimpleList<ActiveEffect*>*；
//      - BSSimpleList 只提供真实存在的成员（begin/end/empty/clear/push_front/Node）。
//  * 全部接口 inline（header-only）：同一个 header 既被被测的 GameBridge.cpp
//    通过同目录 fake PCH.h 引入，也被用例驱动直接引入。
//  * 因此本文件通过 = 「桥接逻辑在当前源码下自洽」，不等于游戏内验证。
// ===========================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// spdlog 桩：GameBridge 只写日志，日志文本不属于桥接逻辑，直接吞掉。
// ---------------------------------------------------------------------------
namespace spdlog {
template <class... A> inline void info(const char*, A&&...) {}
template <class... A> inline void warn(const char*, A&&...) {}
template <class... A> inline void error(const char*, A&&...) {}
}

// ---------------------------------------------------------------------------
// stl::enumeration 桩：真实定义在 SKSE/Impl/PCH.h（CommonLibSSE-NG），是 GameBridge
// 用到的 flags 载体。这里镜像它的公开成员：构造/赋值、explicit operator bool、
// get()/underlying()/set()/reset()/any()/all()/none()（均为可变参数）。
// 未镜像的只有一批宏生成的位运算符（| & ^ 及其复合赋值），fake 内一律用 set()/reset()。
// ---------------------------------------------------------------------------
namespace stl {

template <class Enum, class Underlying = std::underlying_type_t<Enum>>
class enumeration {
public:
    using enum_type = Enum;
    using underlying_type = Underlying;

    static_assert(std::is_enum_v<enum_type>, "enum_type must be an enum");
    static_assert(std::is_integral_v<underlying_type>, "underlying_type must be an integral");

    constexpr enumeration() noexcept = default;

    constexpr enumeration(enum_type value) noexcept : impl_(static_cast<underlying_type>(value)) {}

    template <class... Args>
        requires(std::same_as<Args, enum_type> && ...)
    constexpr enumeration(Args... values) noexcept
        : impl_((static_cast<underlying_type>(values) | ...))
    {
    }

    constexpr enumeration& operator=(enum_type value) noexcept
    {
        impl_ = static_cast<underlying_type>(value);
        return *this;
    }

    [[nodiscard]] explicit constexpr operator bool() const noexcept
    {
        return impl_ != static_cast<underlying_type>(0);
    }
    [[nodiscard]] constexpr enum_type get() const noexcept { return static_cast<enum_type>(impl_); }
    [[nodiscard]] constexpr underlying_type underlying() const noexcept { return impl_; }

    template <class... Args>
        requires(std::same_as<Args, enum_type> && ...)
    constexpr enumeration& set(Args... args) noexcept
    {
        impl_ |= (static_cast<underlying_type>(args) | ...);
        return *this;
    }

    template <class... Args>
        requires(std::same_as<Args, enum_type> && ...)
    constexpr enumeration& reset(Args... args) noexcept
    {
        impl_ &= ~(static_cast<underlying_type>(args) | ...);
        return *this;
    }

    template <class... Args>
        requires(std::same_as<Args, enum_type> && ...)
    [[nodiscard]] constexpr bool any(Args... args) const noexcept
    {
        return (impl_ & (static_cast<underlying_type>(args) | ...)) != static_cast<underlying_type>(0);
    }

    template <class... Args>
        requires(std::same_as<Args, enum_type> && ...)
    [[nodiscard]] constexpr bool all(Args... args) const noexcept
    {
        const auto mask = (static_cast<underlying_type>(args) | ...);
        return (impl_ & mask) == mask;
    }

    template <class... Args>
        requires(std::same_as<Args, enum_type> && ...)
    [[nodiscard]] constexpr bool none(Args... args) const noexcept
    {
        return (impl_ & (static_cast<underlying_type>(args) | ...)) == static_cast<underlying_type>(0);
    }

private:
    underlying_type impl_{0};
};

}  // namespace stl

// ---------------------------------------------------------------------------
// RE 前置声明 + 枚举 + 事件机制骨架
// ---------------------------------------------------------------------------
namespace RE {

using FormID = std::uint32_t;

class TESForm;
class TESObjectREFR;
class Actor;
class PlayerCharacter;
class EffectSetting;
class SpellItem;
class BGSKeyword;
class TESObjectWEAP;
class TESObjectARMO;
class BGSProjectile;
class ActiveEffect;
class MagicCaster;
class TESDataHandler;
class ScriptEventSourceHolder;
class UI;
class Main;
struct TESMagicEffectApplyEvent;
struct TESDeathEvent;
struct TESCombatEvent;
struct MenuOpenCloseEvent;

namespace BSScript {
class IVirtualMachine;
}

enum class ActorValue : std::int32_t { kHealth = 24 };

namespace MagicSystem {
enum class CastingType : std::int32_t { kConstantEffect, kFireAndForget, kConcentration, kScroll };
enum class Delivery : std::int32_t { kSelf, kTouch, kAimed, kTargetActor, kTargetLocation };
enum class CastingSource : std::int32_t { kLeftHand, kRightHand, kOther, kInstant };
}

enum class WEAPON_TYPE : std::int32_t {
    kHandToHandMelee = 0,
    kOneHandSword,
    kOneHandDagger,
    kOneHandAxe,
    kOneHandMace,
    kTwoHandSword,
    kBow,
    kStaff,
    kCrossbow
};

enum class BSEventNotifyControl { kContinue, kStop };

template <class T> class BSTEventSource {};

template <class T>
class BSTEventSink {
public:
    virtual ~BSTEventSink() = default;
    virtual BSEventNotifyControl ProcessEvent(const T*, BSTEventSource<T>*)
    {
        return BSEventNotifyControl::kContinue;
    }
};

// 真实 CommonLib 的 RE::BSSimpleList<T>（RE/B/BSTList.h）：头节点持首元素 + next 链。
// 这里只保留 GameBridge 用得到的部分（范围 for / empty / clear / push_front）。
template <class T>
class BSSimpleList {
public:
    using value_type = T;
    using reference = value_type&;
    using const_reference = const value_type&;

    struct Node {
        value_type item{};
        Node* next{};  // 真实为 stl::observer<Node*>，行为等价
    };

    class iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = T;
        using pointer = value_type*;
        using reference = value_type&;
        using iterator_category = std::forward_iterator_tag;

        iterator() = default;
        explicit iterator(Node* node) : cur_(node) {}
        reference operator*() const { return cur_->item; }
        iterator& operator++()
        {
            cur_ = cur_->next;
            return *this;
        }
        bool operator==(const iterator& rhs) const { return cur_ == rhs.cur_; }
        bool operator!=(const iterator& rhs) const { return !(*this == rhs); }

    private:
        Node* cur_{};
    };

    [[nodiscard]] iterator begin() { return empty() ? end() : iterator(&head_); }
    [[nodiscard]] iterator end() { return iterator(nullptr); }
    [[nodiscard]] bool empty() const { return !head_.next && !head_.item; }

    void clear()
    {
        Node* node = head_.next;
        while (node) {
            Node* next = node->next;
            delete node;
            node = next;
        }
        head_.next = nullptr;
        head_.item = value_type{};
    }

    void push_front(const_reference value)
    {
        if (!head_.item) {
            head_.item = value;
            return;
        }
        head_.next = new Node{head_.item, head_.next};
        head_.item = value;
    }

private:
    Node head_{};
};

class BSFixedString {
public:
    BSFixedString() = default;
    BSFixedString(const char* s) : text(s ? s : "") {}
    BSFixedString(std::string s) : text(std::move(s)) {}
    std::string text;
};

// 真实 RE::NiPointer<T>（RE/N/NiSmartPointer.h，带引用计数的智能指针）。
// 这里只保留 GameBridge / 事件用得到的非拥有语义：模板构造（explicit，仅允许指针可转换）、
// 转换拷贝、get() / -> / explicit operator bool / reset()。不含引用计数与其移动语义。
template <class T>
class NiPointer {
public:
    using element_type = T;

    constexpr NiPointer() noexcept = default;
    constexpr NiPointer(std::nullptr_t) noexcept {}

    template <class Y, std::enable_if_t<std::is_convertible_v<Y*, element_type*>, int> = 0>
    explicit NiPointer(Y* a_ptr) : ptr_(a_ptr)
    {
    }

    NiPointer(const NiPointer& a_rhs) : ptr_(a_rhs.ptr_) {}

    template <class Y, std::enable_if_t<std::is_convertible_v<Y*, element_type*>, int> = 0>
    NiPointer(const NiPointer<Y>& a_rhs) : ptr_(a_rhs.get())
    {
    }

    NiPointer& operator=(const NiPointer& a_rhs)
    {
        ptr_ = a_rhs.ptr_;
        return *this;
    }

    template <class Y, std::enable_if_t<std::is_convertible_v<Y*, element_type*>, int> = 0>
    NiPointer& operator=(const NiPointer<Y>& a_rhs)
    {
        ptr_ = a_rhs.get();
        return *this;
    }

    template <class Y, std::enable_if_t<std::is_convertible_v<Y*, element_type*>, int> = 0>
    void reset(Y* a_ptr)
    {
        ptr_ = a_ptr;
    }

    [[nodiscard]] constexpr element_type* get() const noexcept { return ptr_; }
    [[nodiscard]] constexpr element_type* operator->() const noexcept { return ptr_; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return ptr_ != nullptr; }

private:
    element_type* ptr_{};
};

}  // namespace RE

// ---------------------------------------------------------------------------
// krfake：副作用记录 / 任务队列 / 世界搭建入口（声明；实现见文件末尾）
// ---------------------------------------------------------------------------
namespace krfake {

struct CastRecord {
    RE::SpellItem* spell{};
    RE::Actor* target{};
    float effectiveness{};
    float magnitude{};
};
struct SpellMarkerRecord {
    RE::Actor* actor{};
    RE::SpellItem* spell{};
    bool added{};
};
struct ModEventRecord {
    std::string name;
    float value{};
    RE::Actor* actor{};
};
struct CastReaction {
    RE::SpellItem* spell{};
    RE::EffectSetting* effect{};
};

inline std::vector<CastRecord>& Casts()
{
    static std::vector<CastRecord> value;
    return value;
}
inline std::vector<SpellMarkerRecord>& SpellMarkers()
{
    static std::vector<SpellMarkerRecord> value;
    return value;
}
inline std::vector<ModEventRecord>& ModEvents()
{
    static std::vector<ModEventRecord> value;
    return value;
}
inline std::vector<CastReaction>& CastReactions()
{
    static std::vector<CastReaction> value;
    return value;
}
inline std::vector<std::function<void()>>& Tasks()
{
    static std::vector<std::function<void()>> value;
    return value;
}
inline RE::PlayerCharacter*& PlayerSlot()
{
    static RE::PlayerCharacter* value{};
    return value;
}
inline bool& SpinFlag()
{
    static bool value = false;
    return value;
}
template <class T> inline std::vector<RE::BSTEventSink<T>*>& SinkList()
{
    static std::vector<RE::BSTEventSink<T>*> value;
    return value;
}

// 记录入口（供 fake 的 RE/SKSE 类调用）
void RecordCast(RE::SpellItem* spell, RE::Actor* target, float effectiveness, float magnitude);
void RecordSpellMarker(RE::Actor* actor, RE::SpellItem* spell, bool added);
void RecordModEvent(const std::string& name, float value, RE::Actor* actor);
void ApplyCastReactions(RE::SpellItem* spell, RE::Actor* target);

// 查表（真实 API 的 TESDataHandler::LookupForm / TESForm::LookupByID 落到这里）
RE::TESForm* FindByFullID(RE::FormID id);
RE::TESForm* FindByLocalID(const std::string& plugin, std::uint32_t localID);

// 用例控制面
void ResetScenario();                                            // 每个用例之间清场（表单/对象保留）
RE::ActiveEffect* ApplyEffect(RE::Actor* actor, RE::EffectSetting* effect);
void ClearEffects(RE::Actor* actor);
void OnCastApply(RE::SpellItem* spell, RE::EffectSetting* effect);  // 施放 spell 时给目标挂 effect
void FireMagicEffect(const RE::TESMagicEffectApplyEvent& event);
void FireDeath(const RE::TESDeathEvent& event);
void FireCombat(const RE::TESCombatEvent& event);
int DrainAll(int maxSteps = 10000);   // 跑到队列空；超过 maxSteps 判定同帧自旋
void RunFrame();                      // 只跑进入时已入队的任务（等价一帧）
int CastCount(RE::SpellItem* spell);
int MarkerCount(RE::SpellItem* spell);
int EventCount(const std::string& name);

// 世界搭建（整进程只搭一次；表单对象稳定存活，供 GameBridge 单例长期持有）
int AddPlugin(const std::string& name);
RE::FormID FullID(int plugin, std::uint32_t localID);
template <class T> T* AddRecord(int plugin, std::uint32_t localID);
RE::EffectSetting* AddEffect(int plugin, std::uint32_t localID);
RE::SpellItem* AddSpell(int plugin, std::uint32_t localID);
RE::BGSKeyword* AddKeyword(int plugin, std::uint32_t localID);
RE::TESObjectWEAP* AddWeapon(int plugin, std::uint32_t localID);
RE::TESObjectARMO* AddArmor(int plugin, std::uint32_t localID);
RE::Actor* AddActor(RE::FormID fullID);
RE::PlayerCharacter* AddPlayer(RE::FormID fullID);

}  // namespace krfake

// ---------------------------------------------------------------------------
// RE 类型定义（只含 GameBridge 实际调用的成员）
// ---------------------------------------------------------------------------
namespace RE {

class TESForm {
public:
    virtual ~TESForm() = default;

    FormID GetFormID() const { return formID_; }
    void SetFormIDForTest(FormID id) { formID_ = id; }  // fake-only

    template <class T> T* As() { return dynamic_cast<T*>(this); }
    template <class T> const T* As() const { return dynamic_cast<const T*>(this); }

    template <class T = TESForm> static T* LookupByID(FormID id)
    {
        return dynamic_cast<T*>(krfake::FindByFullID(id));
    }

    // 真实签名是 const BGSKeyword*；fake 内表单实例唯一，指针比较等价于 FormID 比较。
    bool HasKeyword(const BGSKeyword* keyword) const
    {
        if (!keyword) {
            return false;
        }
        for (const BGSKeyword* item : keywords_) {
            if (item == keyword) {
                return true;
            }
        }
        return false;
    }
    void AddKeywordForTest(BGSKeyword* keyword)  // fake-only
    {
        if (keyword) {
            keywords_.push_back(keyword);
        }
    }

protected:
    FormID formID_{};
    std::vector<const BGSKeyword*> keywords_;
};

class TESObjectREFR : public TESForm {};
class TESObjectARMO : public TESForm {};
class BGSProjectile : public TESForm {};
class BGSKeyword : public TESForm {};

class TESObjectWEAP : public TESForm {
public:
    WEAPON_TYPE weaponType{WEAPON_TYPE::kOneHandAxe};
    WEAPON_TYPE GetWeaponType() const { return weaponType; }
};

class EffectSetting : public TESForm {
public:
    // 真实为 EffectSetting::EffectSettingData（RE/E/EffectSetting.h）：flags 是
    // stl::enumeration<Flag, std::uint32_t>，IsHostile/IsDetrimental 均由 flags 派生。
    struct EffectSettingData {
        enum class Flag {
            kNone = 0,
            kHostile = 1 << 0,
            kRecover = 1 << 1,
            kDetrimental = 1 << 2,
            kSnapToNavMesh = 1 << 3,
            kNoHitEvent = 1 << 4,
            kDispelWithKeywords = 1 << 8,
            kNoDuration = 1 << 9,
            kNoMagnitude = 1 << 10,
            kNoArea = 1 << 11,
            kFXPersist = 1 << 12,
            kGoryVisuals = 1 << 14,
            kHideInUI = 1 << 15,
            kNoRecast = 1 << 17
        };

        stl::enumeration<Flag, std::uint32_t> flags;
        ActorValue primaryAV{ActorValue::kHealth};
    };

    EffectSettingData data;

    [[nodiscard]] bool IsHostile() const noexcept
    {
        return data.flags.all(EffectSettingData::Flag::kHostile);
    }
    [[nodiscard]] bool IsDetrimental() const noexcept
    {
        return data.flags.all(EffectSettingData::Flag::kDetrimental);
    }
};

class ActiveEffect {
public:
    // 真实位值见 RE/A/ActiveEffect.h（kInactive = 1<<15、kDispelled = 1<<18）。
    enum class Flag {
        kHasConditions = 1 << 7,
        kEnchanting = 1 << 8,
        kRecovers = 1 << 9,
        kDual = 1 << 12,
        kInactive = 1 << 15,
        kDispelled = 1 << 18
    };

    stl::enumeration<Flag, std::uint32_t> flags;
    TESForm* baseObject{};
    TESForm* GetBaseObject() const { return baseObject; }
};

// 真实 RE::MagicTarget（RE/M/MagicTarget.h）：abstract，07 号虚函数即 GetActiveEffectList。
// 注意签名非 const、返回 RE::BSSimpleList<RE::ActiveEffect*>*。
class MagicTarget {
public:
    virtual ~MagicTarget() = default;
    virtual BSSimpleList<ActiveEffect*>* GetActiveEffectList() = 0;
};

// fake 内部的具体实现：真实里是 Actor 以 MagicTarget 为基类子对象（AE 下偏移 0xA0），
// 而 Actor 自身的 GetActiveEffectList() override 被 #ifndef ENABLE_SKYRIM_AE 排除。
// 为了让 fake 与 AE-only 构建一样「只能走 AsMagicTarget()」，这里刻意让 Actor **不继承**
// MagicTarget，而是持有本对象：actor->GetActiveEffectList() 在 fake 里同样编译不过，
// 从而继续暴露这一类真实编译问题（见文件头说明）。
class FakeMagicTarget : public MagicTarget {
public:
    BSSimpleList<ActiveEffect*> effects;
    BSSimpleList<ActiveEffect*>* GetActiveEffectList() override { return &effects; }
};

// 真实 RE::Effect（RE/E/Effect.h）：struct，内嵌 EFIT（magnitude/area/duration）。
struct Effect {
    struct EffectItem  // EFIT
    {
        float magnitude{};
        std::uint32_t area{};
        std::uint32_t duration{};
    };

    EffectItem effectItem;
    EffectSetting* baseEffect{};  // EFID

    // 真实实现（vendor/CommonLibSSE-NG/src/RE/E/Effect.cpp）：Get* 会先看 MGEF 的
    // kNoMagnitude / kNoArea / kNoDuration 再取 EFIT；这里照此镜像（额外加 baseEffect 空保护）。
    [[nodiscard]] float GetMagnitude() const
    {
        if (baseEffect && baseEffect->data.flags.any(EffectSetting::EffectSettingData::Flag::kNoMagnitude)) {
            return 0.0f;
        }
        return effectItem.magnitude;
    }
    [[nodiscard]] std::uint32_t GetArea() const
    {
        if (baseEffect && baseEffect->data.flags.any(EffectSetting::EffectSettingData::Flag::kNoArea)) {
            return 0;
        }
        return effectItem.area;
    }
    [[nodiscard]] std::uint32_t GetDuration() const
    {
        if (baseEffect && baseEffect->data.flags.any(EffectSetting::EffectSettingData::Flag::kNoDuration)) {
            return 0;
        }
        return effectItem.duration;
    }
    [[nodiscard]] bool IsHostile() const { return baseEffect && baseEffect->IsHostile(); }
};

// 真实 MagicItem 持有 effects（RE/M/MagicItem.h: BSTArray<Effect*>），SpellItem 派生自它。
class MagicItem : public TESForm {
public:
    // 真实容器是 RE::BSTArray<RE::Effect*>；这里用 std::vector 充当，只需范围 for 语义。
    std::vector<Effect*> effects;
};

class SpellItem : public MagicItem {
public:
    MagicSystem::CastingType castingType{MagicSystem::CastingType::kFireAndForget};
    MagicSystem::Delivery delivery{MagicSystem::Delivery::kAimed};

    MagicSystem::CastingType GetCastingType() const { return castingType; }
    MagicSystem::Delivery GetDelivery() const { return delivery; }
};

class MagicCaster {
public:
    void CastSpellImmediate(SpellItem* spell, bool leftHand, Actor* target, float effectiveness,
        bool dualCast, float magnitude, Actor* caster);
};

class Actor : public TESObjectREFR {
public:
    // ---- 用例可控的假体状态（fake-only，真实 API 里由引擎维护） ----
    bool dead{};
    bool inCombat{};
    bool teammate{};
    bool hostile{};
    Actor* commander{};
    TESForm* equippedRight{};
    TESForm* equippedLeft{};
    std::vector<SpellItem*> spells;

    bool IsDead() const { return dead; }
    bool IsInCombat() const { return inCombat; }
    bool IsPlayerTeammate() const { return teammate; }
    bool IsHostileToActor(Actor*) const { return hostile; }
    NiPointer<Actor> GetCommandingActor() const { return NiPointer<Actor>(commander); }
    TESForm* GetEquippedObject(bool leftHand) const { return leftHand ? equippedLeft : equippedRight; }

    // 真实签名：Actor::AsMagicTarget() noexcept / const 版本返回 const MagicTarget*（0x98/0xA0）。
    // 效果列表只能经此进入（AE-only 下 Actor 自身没有可用的 GetActiveEffectList）。
    [[nodiscard]] MagicTarget* AsMagicTarget() noexcept { return &magicTarget_; }
    [[nodiscard]] const MagicTarget* AsMagicTarget() const noexcept { return &magicTarget_; }

    bool HasSpell(const SpellItem* spell) const
    {
        for (const SpellItem* item : spells) {
            if (item == spell) {
                return true;
            }
        }
        return false;
    }
    void AddSpell(SpellItem* spell)
    {
        if (spell && !HasSpell(spell)) {
            spells.push_back(spell);
            krfake::RecordSpellMarker(this, spell, true);
        }
    }
    void RemoveSpell(SpellItem* spell)
    {
        if (!spell) {
            return;
        }
        for (auto it = spells.begin(); it != spells.end(); ++it) {
            if (*it == spell) {
                spells.erase(it);
                krfake::RecordSpellMarker(this, spell, false);
                return;
            }
        }
    }
    MagicCaster* GetMagicCaster(RE::MagicSystem::CastingSource) { return &caster_; }

private:
    FakeMagicTarget magicTarget_;
    MagicCaster caster_;
};

class PlayerCharacter : public Actor {
public:
    static PlayerCharacter* GetSingleton() { return krfake::PlayerSlot(); }
};

// 三个事件按真实 header 的成员名/类型/顺序镜像（RE/T/TESMagicEffectApplyEvent.h 等）：
// 指针成员真实为 NiPointer<TESObjectREFR>，不是 Actor*。
struct TESMagicEffectApplyEvent {
    NiPointer<TESObjectREFR> target;
    NiPointer<TESObjectREFR> caster;
    FormID magicEffect{};
};

struct TESDeathEvent {
    NiPointer<TESObjectREFR> actorDying;
    NiPointer<TESObjectREFR> actorKiller;
    bool dead{};  // false = 濒死阶段，true = 真正死亡（由引擎置位）
};

struct TESCombatEvent {
    NiPointer<TESObjectREFR> actor;
};

// 真实 RE::MenuOpenCloseEvent（RE/U/UIMessage.h 旁）：成员名按真实 header 镜像。
struct MenuOpenCloseEvent {
    BSFixedString menuName;
    bool opening{};
};

class TESDataHandler {
public:
    static TESDataHandler* GetSingleton()
    {
        static TESDataHandler instance;
        return &instance;
    }

    template <class T = TESForm> T* LookupForm(std::uint32_t localID, std::string_view plugin)
    {
        return dynamic_cast<T*>(krfake::FindByLocalID(std::string(plugin), localID));
    }
};

class ScriptEventSourceHolder {
public:
    static ScriptEventSourceHolder* GetSingleton()
    {
        static ScriptEventSourceHolder instance;
        return &instance;
    }

    template <class T> void AddEventSink(BSTEventSink<T>* sink)
    {
        krfake::SinkList<T>().push_back(sink);
    }
};

// 真实 RE::UI（RE/U/UI.h）：GameIsPaused / IsItemMenuOpen / IsApplicationMenuOpen /
// IsModalMenuOpen 均为真实存在的 const 成员；AddEventSink 是模板成员。用例可控开关暴露为成员。
class UI {
public:
    static UI* GetSingleton()
    {
        static UI instance;
        return &instance;
    }

    template <class T> void AddEventSink(BSTEventSink<T>* sink)
    {
        krfake::SinkList<T>().push_back(sink);
    }

    bool GameIsPaused() const { return paused; }
    bool IsItemMenuOpen() const { return itemMenu; }
    bool IsApplicationMenuOpen() const { return appMenu; }
    bool IsModalMenuOpen() const { return modalMenu; }
    // 真实签名按名查表；fake 只支持桥接关心的三个名字。
    bool IsMenuOpen(const BSFixedString& name) const
    {
        if (name.text == "Loading Menu") {
            return loadingMenu;
        }
        if (name.text == "Console") {
            return console;
        }
        if (name.text == "Dialogue Menu") {
            return dialogueMenu;
        }
        return false;
    }

    bool paused{};
    bool itemMenu{};
    bool appMenu{};
    bool modalMenu{};
    bool loadingMenu{};
    bool console{};
    bool dialogueMenu{};
};

// 真实 RE::Main（RE/M/Main.h）：freezeTime 与 gameActive 是真实存在的 bool 成员。
class Main {
public:
    static Main* GetSingleton()
    {
        static Main instance;
        return &instance;
    }

    bool freezeTime{};
    bool gameActive{true};
};

}  // namespace RE

// ---------------------------------------------------------------------------
// SKSE 桩：任务接口 + mod callback 事件源
// ---------------------------------------------------------------------------
namespace SKSE {

class TaskInterface {
public:
    void AddTask(std::function<void()> task)
    {
        if (task) {
            krfake::Tasks().push_back(std::move(task));
        }
    }
};

struct ModCallbackEvent {
    RE::BSFixedString eventName;
    RE::BSFixedString sender;
    float value{};
    RE::Actor* actor{};
};

class ModCallbackEventSource {
public:
    void SendEvent(ModCallbackEvent* event)
    {
        if (event) {
            krfake::RecordModEvent(event->eventName.text, event->value, event->actor);
        }
    }
};

inline TaskInterface* GetTaskInterface()
{
    static TaskInterface instance;
    return &instance;
}

inline ModCallbackEventSource* GetModCallbackEventSource()
{
    static ModCallbackEventSource instance;
    return &instance;
}

}  // namespace SKSE

// ---------------------------------------------------------------------------
// krfake 实现
// ---------------------------------------------------------------------------
namespace krfake {

namespace detail {

struct Registry {
    std::unordered_map<RE::FormID, RE::TESForm*> byFullID;
    std::unordered_map<std::string, std::unordered_map<std::uint32_t, RE::TESForm*>> byLocalID;
    std::vector<std::string> pluginNames;
    std::vector<std::unique_ptr<RE::TESForm>> formOwner;
    std::vector<std::unique_ptr<RE::Actor>> actorOwner;
    std::vector<std::unique_ptr<RE::PlayerCharacter>> playerOwner;
    std::vector<std::unique_ptr<RE::ActiveEffect>> effectOwner;
};

inline Registry& Reg()
{
    static Registry value;
    return value;
}

}  // namespace detail

inline RE::TESForm* FindByFullID(RE::FormID id)
{
    auto& registry = detail::Reg();
    const auto it = registry.byFullID.find(id);
    return it == registry.byFullID.end() ? nullptr : it->second;
}

inline RE::TESForm* FindByLocalID(const std::string& plugin, std::uint32_t localID)
{
    auto& registry = detail::Reg();
    const auto pluginIt = registry.byLocalID.find(plugin);
    if (pluginIt == registry.byLocalID.end()) {
        return nullptr;
    }
    const auto formIt = pluginIt->second.find(localID);
    return formIt == pluginIt->second.end() ? nullptr : formIt->second;
}

inline void RecordCast(RE::SpellItem* spell, RE::Actor* target, float effectiveness, float magnitude)
{
    Casts().push_back(CastRecord{spell, target, effectiveness, magnitude});
}

inline void RecordSpellMarker(RE::Actor* actor, RE::SpellItem* spell, bool added)
{
    SpellMarkers().push_back(SpellMarkerRecord{actor, spell, added});
}

inline void RecordModEvent(const std::string& name, float value, RE::Actor* actor)
{
    ModEvents().push_back(ModEventRecord{name, value, actor});
}

inline void ApplyCastReactions(RE::SpellItem* spell, RE::Actor* target)
{
    for (const CastReaction& reaction : CastReactions()) {
        if (reaction.spell == spell && reaction.effect) {
            ApplyEffect(target, reaction.effect);
        }
    }
}

inline RE::ActiveEffect* ApplyEffect(RE::Actor* actor, RE::EffectSetting* effect)
{
    if (!actor || !effect) {
        return nullptr;
    }
    auto owned = std::make_unique<RE::ActiveEffect>();
    owned->baseObject = effect;
    RE::ActiveEffect* raw = owned.get();
    detail::Reg().effectOwner.push_back(std::move(owned));
    // 与 GameBridge 相同的访问路径：AsMagicTarget()->GetActiveEffectList()
    actor->AsMagicTarget()->GetActiveEffectList()->push_front(raw);
    return raw;
}

inline void ClearEffects(RE::Actor* actor)
{
    if (actor) {
        actor->AsMagicTarget()->GetActiveEffectList()->clear();
    }
}

inline void OnCastApply(RE::SpellItem* spell, RE::EffectSetting* effect)
{
    CastReactions().push_back(CastReaction{spell, effect});
}

inline void FireMagicEffect(const RE::TESMagicEffectApplyEvent& event)
{
    // 复制一份容器：sink 回调里可能再次注册（真实引擎也不会遍历中被改的容器）。
    const auto sinks = SinkList<RE::TESMagicEffectApplyEvent>();
    for (auto* sink : sinks) {
        if (sink) {
            sink->ProcessEvent(&event, nullptr);
        }
    }
}

inline void FireDeath(const RE::TESDeathEvent& event)
{
    const auto sinks = SinkList<RE::TESDeathEvent>();
    for (auto* sink : sinks) {
        if (sink) {
            sink->ProcessEvent(&event, nullptr);
        }
    }
}

inline void FireCombat(const RE::TESCombatEvent& event)
{
    const auto sinks = SinkList<RE::TESCombatEvent>();
    for (auto* sink : sinks) {
        if (sink) {
            sink->ProcessEvent(&event, nullptr);
        }
    }
}

inline void FireMenuOpenClose(const RE::MenuOpenCloseEvent& event)
{
    const auto sinks = SinkList<RE::MenuOpenCloseEvent>();
    for (auto* sink : sinks) {
        if (sink) {
            sink->ProcessEvent(&event, nullptr);
        }
    }
}

inline int DrainAll(int maxSteps)
{
    int ran = 0;
    auto& queue = Tasks();
    while (!queue.empty()) {
        if (ran >= maxSteps) {
            SpinFlag() = true;  // 任务在自己的执行里无限重排 = 同帧自旋
            break;
        }
        auto task = std::move(queue.front());
        queue.erase(queue.begin());
        ++ran;
        if (task) {
            task();
        }
    }
    return ran;
}

inline void RunFrame()
{
    // 只跑进入时已入队的任务；期间新入队的留给下一帧（等价 SKSE 单帧任务泵）。
    std::vector<std::function<void()>> batch;
    batch.swap(Tasks());
    for (auto& task : batch) {
        if (task) {
            task();
        }
    }
}

inline int CastCount(RE::SpellItem* spell)
{
    int count = 0;
    for (const CastRecord& record : Casts()) {
        if (record.spell == spell) {
            ++count;
        }
    }
    return count;
}

inline int MarkerCount(RE::SpellItem* spell)
{
    int count = 0;
    for (const SpellMarkerRecord& record : SpellMarkers()) {
        if (record.spell == spell && record.added) {
            ++count;
        }
    }
    return count;
}

inline int EventCount(const std::string& name)
{
    int count = 0;
    for (const ModEventRecord& record : ModEvents()) {
        if (record.name == name) {
            ++count;
        }
    }
    return count;
}

inline void ResetScenario()
{
    Casts().clear();
    SpellMarkers().clear();
    ModEvents().clear();
    CastReactions().clear();
    Tasks().clear();
    SpinFlag() = false;

    if (auto* ui = RE::UI::GetSingleton()) {
        ui->paused = false;
        ui->itemMenu = false;
        ui->appMenu = false;
        ui->modalMenu = false;
        ui->loadingMenu = false;
        ui->console = false;
        ui->dialogueMenu = false;
    }
    if (auto* main = RE::Main::GetSingleton()) {
        main->freezeTime = false;
        main->gameActive = true;
    }

    auto& registry = detail::Reg();
    for (auto& actor : registry.actorOwner) {
        actor->AsMagicTarget()->GetActiveEffectList()->clear();  // 先断开列表，再释放对象
        actor->dead = false;
        actor->inCombat = false;
        actor->teammate = false;
        actor->hostile = false;
        actor->commander = nullptr;
        actor->equippedRight = nullptr;
        actor->equippedLeft = nullptr;
        actor->spells.clear();
    }
    for (auto& player : registry.playerOwner) {
        player->AsMagicTarget()->GetActiveEffectList()->clear();
        player->dead = false;
        player->inCombat = false;
        player->teammate = false;
        player->hostile = false;
        player->commander = nullptr;
        player->equippedRight = nullptr;
        player->equippedLeft = nullptr;
        player->spells.clear();
    }
    registry.effectOwner.clear();
    // 事件 sink 不清：GameBridge 只在首次 Initialize 注册一次，且单例长期存活。
}

inline int AddPlugin(const std::string& name)
{
    auto& registry = detail::Reg();
    registry.pluginNames.push_back(name);
    return static_cast<int>(registry.pluginNames.size()) - 1;
}

inline RE::FormID FullID(int plugin, std::uint32_t localID)
{
    // 模拟 load order 偏移：全 FormID = (pluginIndex + 1) << 24 | localID。
    return ((static_cast<RE::FormID>(plugin) + 1u) << 24) | (localID & 0x00FFFFFFu);
}

template <class T> T* AddRecord(int plugin, std::uint32_t localID)
{
    auto& registry = detail::Reg();
    auto object = std::make_unique<T>();
    const RE::FormID id = FullID(plugin, localID);
    object->SetFormIDForTest(id);
    T* raw = object.get();
    auto& locals = registry.byLocalID[registry.pluginNames[static_cast<std::size_t>(plugin)]];
    if (locals.count(localID) != 0 || registry.byFullID.count(id) != 0) {
        // 同一 (插件, localID) 重复注册会悄悄换掉表单实例，让桥接单例持有的指针与用例持有的
        // 指针分叉 —— 这是脚手架搭建错误，直接硬失败而不是制造假失败。
        std::fprintf(stderr, "krfake: duplicate record (%X in plugin #%d)\n", localID, plugin);
        std::abort();
    }
    registry.byFullID[id] = raw;
    locals[localID] = raw;
    registry.formOwner.push_back(std::move(object));
    return raw;
}

inline RE::EffectSetting* AddEffect(int plugin, std::uint32_t localID)
{
    return AddRecord<RE::EffectSetting>(plugin, localID);
}
inline RE::SpellItem* AddSpell(int plugin, std::uint32_t localID)
{
    return AddRecord<RE::SpellItem>(plugin, localID);
}
inline RE::BGSKeyword* AddKeyword(int plugin, std::uint32_t localID)
{
    return AddRecord<RE::BGSKeyword>(plugin, localID);
}
inline RE::TESObjectWEAP* AddWeapon(int plugin, std::uint32_t localID)
{
    return AddRecord<RE::TESObjectWEAP>(plugin, localID);
}
inline RE::TESObjectARMO* AddArmor(int plugin, std::uint32_t localID)
{
    return AddRecord<RE::TESObjectARMO>(plugin, localID);
}

inline RE::Actor* AddActor(RE::FormID fullID)
{
    auto& registry = detail::Reg();
    auto object = std::make_unique<RE::Actor>();
    object->SetFormIDForTest(fullID);
    RE::Actor* raw = object.get();
    registry.byFullID[fullID] = raw;
    registry.actorOwner.push_back(std::move(object));
    return raw;
}

inline RE::PlayerCharacter* AddPlayer(RE::FormID fullID)
{
    auto& registry = detail::Reg();
    auto object = std::make_unique<RE::PlayerCharacter>();
    object->SetFormIDForTest(fullID);
    RE::PlayerCharacter* raw = object.get();
    registry.byFullID[fullID] = raw;
    registry.playerOwner.push_back(std::move(object));
    PlayerSlot() = raw;
    return raw;
}

}  // namespace krfake

// ---------------------------------------------------------------------------
// RE::MagicCaster 实现（放在最后，需要 Actor 完整类型）
// ---------------------------------------------------------------------------
namespace RE {

inline void MagicCaster::CastSpellImmediate(SpellItem* spell, bool, Actor* target, float effectiveness,
    bool, float magnitude, Actor* caster)
{
    // target 为 null 表示非自指施法；按真实语义取施法者身上落效果（reaction 用）。
    krfake::RecordCast(spell, target, effectiveness, magnitude);
    krfake::ApplyCastReactions(spell, target ? target : caster);
}

}  // namespace RE
