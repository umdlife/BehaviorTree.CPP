#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>

#include "behaviortree_cpp/basic_types.h"
#include "behaviortree_cpp/contrib/json.hpp"
#include "behaviortree_cpp/utils/safe_any.hpp"
#include "behaviortree_cpp/exceptions.h"
#include "behaviortree_cpp/utils/locked_reference.hpp"

namespace BT
{

/// This type contains a pointer to Any, protected
/// with a locked mutex as long as the object is in scope
using AnyPtrLocked = LockedPtr<Any>;

/**
 * @brief The Blackboard is the mechanism used by BehaviorTrees to exchange
 * typed data.
 */
class Blackboard
{
public:
  using Ptr = std::shared_ptr<Blackboard>;

protected:
  // This is intentionally protected. Use Blackboard::create instead
  Blackboard(Blackboard::Ptr parent) : parent_bb_(parent)
  {}

public:
  struct Entry
  {
    Any value;
    TypeInfo info;
    StringConverter string_converter;
    mutable std::mutex entry_mutex;

    Entry(const TypeInfo& _info) : info(_info)
    {}
  };

  /** Use this static method to create an instance of the BlackBoard
    *   to share among all your NodeTrees.
    */
  static Blackboard::Ptr create(Blackboard::Ptr parent = {})
  {
    return std::shared_ptr<Blackboard>(new Blackboard(parent));
  }

  virtual ~Blackboard() = default;

  void enableAutoRemapping(bool remapping);

  [[nodiscard]] const std::shared_ptr<Entry> getEntry(const std::string& key) const;

  [[nodiscard]] std::shared_ptr<Blackboard::Entry> getEntry(const std::string& key);

  [[nodiscard]] AnyPtrLocked getAnyLocked(const std::string& key);

  [[nodiscard]] AnyPtrLocked getAnyLocked(const std::string& key) const;

  [[deprecated("Use getAnyLocked instead")]] const Any*
  getAny(const std::string& key) const;

  [[deprecated("Use getAnyLocked instead")]] Any* getAny(const std::string& key);

  /** Return true if the entry with the given key was found.
   *  Note that this method may throw an exception if the cast to T failed.
   */
  template <typename T>
  [[nodiscard]] bool get(const std::string& key, T& value) const;

  /**
   * Version of get() that throws if it fails.
   */
  template <typename T>
  [[nodiscard]] T get(const std::string& key) const;

  /// Update the entry with the given key
  template <typename T>
  void set(const std::string& key, const T& value);

  void unset(const std::string& key);

  [[nodiscard]] const TypeInfo* entryInfo(const std::string& key);

  void addSubtreeRemapping(StringView internal, StringView external);

  void debugMessage() const;

  [[nodiscard]] std::vector<StringView> getKeys() const;

  void clear();

  [[deprecated("Use getAnyLocked to access safely an Entry")]] std::recursive_mutex&
  entryMutex() const;

  void createEntry(const std::string& key, const TypeInfo& info);

  /**
   * @brief cloneInto copies the values of the entries
   * into another blackboard.  Known limitations:
   *
   * - it doesn't update the remapping in dst
   * - it doesn't change the parent blackboard os dst
   *
   * @param dst destination, i.e. blackboard to be updated
   */
  void cloneInto(Blackboard& dst) const;

private:
  mutable std::mutex mutex_;
  mutable std::recursive_mutex entry_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Entry>> storage_;
  std::weak_ptr<Blackboard> parent_bb_;
  std::unordered_map<std::string, std::string> internal_to_external_;

  std::shared_ptr<Entry> createEntryImpl(const std::string& key, const TypeInfo& info);

  bool autoremapping_ = false;
};

/**
 * @brief ExportBlackboardToJSON will create a JSON
 * that contains the current values of the blackboard.
 * Complex types must be registered with JsonExporter::get()
 */
nlohmann::json ExportBlackboardToJSON(const Blackboard& blackboard);

/**
 * @brief ImportBlackboardFromJSON will append elements to the blackboard,
 * using the values parsed from the JSON file created using ExportBlackboardToJSON.
 * Complex types must be registered with JsonExporter::get()
 */
void ImportBlackboardFromJSON(const nlohmann::json& json, Blackboard& blackboard);

//------------------------------------------------------

template <typename T>
inline T Blackboard::get(const std::string& key) const
{
  if(auto any_ref = getAnyLocked(key))
  {
    const auto& any = any_ref.get();
    if(any->empty())
    {
      throw RuntimeError("Blackboard::get() error. Entry [", key,
                         "] hasn't been initialized, yet");
    }
    return any_ref.get()->cast<T>();
  }
  else
  {
    throw RuntimeError("Blackboard::get() error. Missing key [", key, "]");
  }
}

inline void Blackboard::unset(const std::string& key)
{
  std::unique_lock lock(mutex_);

  // check local storage
  auto it = storage_.find(key);
  if(it == storage_.end())
  {
    // No entry, nothing to do.
    return;
  }

  storage_.erase(it);
}

template <typename T>
inline void Blackboard::set(const std::string& key, const T& value)
{
  std::unique_lock lock(mutex_);

  // check local storage
  auto it = storage_.find(key);
  if(it == storage_.end())
  {
    // create a new entry
    Any new_value(value);
    lock.unlock();
    std::shared_ptr<Blackboard::Entry> entry;
    // if a new generic port is created with a string, it's type should be AnyTypeAllowed
    if constexpr(std::is_same_v<std::string, T>)
    {
      entry = createEntryImpl(key, PortInfo(PortDirection::INOUT));
    }
    else
    {
      PortInfo new_port(PortDirection::INOUT, new_value.type(),
                        GetAnyFromStringFunctor<T>());
      entry = createEntryImpl(key, new_port);
    }
    lock.lock();

    storage_.insert({ key, entry });
    entry->value = new_value;
  }
  else
  {
    // this is not the first time we set this entry, we need to check
    // if the type is the same or not.
    Entry& entry = *it->second;
    std::scoped_lock scoped_lock(entry.entry_mutex);

    Any& previous_any = entry.value;

    Any new_value(value);

    // special case: entry exists but it is not strongly typed... yet
    if(!entry.info.isStronglyTyped())
    {
      // Use the new type to create a new entry that is strongly typed.
      entry.info = TypeInfo::Create<T>();
      previous_any = std::move(new_value);
      return;
    }

    std::type_index previous_type = entry.info.type();

    // check type mismatch
    if(previous_type != std::type_index(typeid(T)) && previous_type != new_value.type())
    {
      bool mismatching = true;
      if(std::is_constructible<StringView, T>::value)
      {
        Any any_from_string = entry.info.parseString(value);
        if(any_from_string.empty() == false)
        {
          mismatching = false;
          new_value = std::move(any_from_string);
        }
      }
      // check if we are doing a safe cast between numbers
      // for instance, it is safe to use int(100) to set
      // a uint8_t port, but not int(-42) or int(300)
      if constexpr(std::is_arithmetic_v<T>)
      {
        if(mismatching && isCastingSafe(previous_type, value))
        {
          mismatching = false;
        }
      }

      if(mismatching)
      {
        debugMessage();

        auto msg = StrCat("Blackboard::set(", key,
                          "): once declared, "
                          "the type of a port shall not change. "
                          "Previously declared type [",
                          BT::demangle(previous_type), "], current type [",
                          BT::demangle(typeid(T)), "]");
        throw LogicError(msg);
      }
    }
    // if doing set<BT::Any>, skip type check
    if constexpr(std::is_same_v<Any, T>)
    {
      previous_any = new_value;
    }
    else
    {
      // copy only if the type is compatible
      new_value.copyInto(previous_any);
    }
  }
}

template <typename T>
inline bool Blackboard::get(const std::string& key, T& value) const
{
  if(auto any_ref = getAnyLocked(key))
  {
    value = any_ref.get()->cast<T>();
    return true;
  }
  return false;
}

}  // namespace BT
