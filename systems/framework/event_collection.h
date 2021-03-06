#pragma once

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "drake/common/default_scalars.h"
#include "drake/common/drake_copyable.h"
#include "drake/systems/framework/context.h"
#include "drake/systems/framework/event.h"
#include "drake/systems/framework/state.h"

namespace drake {
namespace systems {

// TODO(siyuan): move these to a separate module doxygen file.

/**
 * There are three concrete event types for any System: publish, discrete
 * state update, and unrestricted state update, listed in order of increasing
 * ability to change the state (i.e., zero to all). EventCollection is an
 * abstract base class that stores simultaneous events *of the same type* that
 * occur *at the same time* (i.e., simultaneous events).
 *
 * For each concrete event type, the LeafSystem API provides a unique
 * customizable function for processing all simultaneous events of that type,
 * e.g.
 * LeafSystem::DoPublish(const Context&, const vector<const PublishEvent*>&)
 * for publish events, where the second argument represents all of the publish
 * events that occur simultaneously for that leaf system. The default
 * implementation processes the events (i.e., call their callback functions)
 * in the order in which they are stored in the second argument.
 * The developer of new classes derived from LeafSystem is responsible for
 * overriding such functions if the custom LeafSystem behavior depends on the
 * order in which events are processed. For example, suppose two publish events
 * are being processed, `events = {per-step publish, periodic publish}`.
 * Depending on the desired behavior, the developer has the freedom to ignore
 * both events, perform only one publish action, or perform both publish actions
 * in any arbitrary order. The System and Diagram API provide only dispatch
 * mechanisms that delegate actual event handling to the
 * constituent leaf systems. The Simulator promises that for each set of
 * simultaneous events of the same type, the public event handling method
 * (e.g. System::Publish(context, publish_events)) will be invoked exactly once.
 *
 * The System API provides several functions for customizable event generation
 * such as System::DoCalcNextUpdateTime() or System::DoGetPerStepEvents().
 * These functions can return any number of events of arbitrary types, and the
 * resulting events are stored in separate CompositeEventCollection instances.
 * Before calling the event handlers, all of these CompositeEventCollection
 * objects must be merged to generate a complete set of simultaneous events.
 * Then, only events of the appropriate type are passed to the event handlers.
 * e.g. sys.Publish(context, combined_event_collection.get_publish_events()).
 * For example, the Simulator executes this collation process when it is
 * applied to simulate a system.
 *
 * Here is a complete example. For some LeafSystem `sys` at time `t`, its
 * System::DoCalcNextUpdateTime() generates the following
 * CompositeEventCollection (`events1`):
 * <pre>
 *   PublishEvent: {event1(kPeriodic, callback1)}
 *   DiscreteUpdateEvent: {event2(kPeriodic, callback2)}
 *   UnrestrictedUpdateEvent: {}
 * </pre>
 * This LeafSystem also desires per-step event processing(`events2`),
 * generated by its implementation of System::DoGetPerStepEvents():
 * <pre>
 *   PublishEvent: {event3(kPerStep, callback3)}
 *   DiscreteUpdateEvent: {}
 *   UnrestrictedUpdateEvent: {event4(kPerStep,callback4)}
 * </pre>
 * These collections of "simultaneous" events, `events1` and `events2`, are then
 * merged into the composite event collection `all_events`:
 * <pre>
 *   PublishEvent: {event1, event3}
 *   DiscreteUpdateEvent: {event2}
 *   UnrestrictedUpdateEvent: {event4}
 * </pre>
 * This heterogeneous event collection can be processed by calling
 * the appropriate handler on the appropriate homogeneous subcollection:
 * <pre>
 *   sys.CalcUnrestrictedUpdate(context,
 *       all_events.get_unrestricted_update_events(), state);
 *   sys.CalcDiscreteVariableUpdates(context,
 *       all_events.get_discrete_update_events(), discrete_state);
 *   sys.Publish(context, all_events.get_publish_events())
 * </pre>
 * For a LeafSystem, this is equivalent to (by expanding the dispatch mechanisms
 * in the System API):
 * <pre>
 *   sys.DoCalcUnrestrictedUpdate(context, {event4}, state);
 *   sys.DoCalcDiscreteVariableUpdates(context, {event2}, discrete_state);
 *   sys.DoPublish(context, {event1, event3})
 * </pre>
 *
 * @tparam EventType a concrete derived type of Event (e.g., PublishEvent).
 */
template <typename EventType>
class EventCollection {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(EventCollection)

  virtual ~EventCollection() {}

  /**
   * Clears all the events maintained by `this` then adds all of the events in
   * `other` to this.
   */
  void SetFrom(const EventCollection<EventType>& other) {
    Clear();
    AddToEnd(other);
  }

  /**
   * Adds all of `other`'s events to the end of `this`.
   */
  void AddToEnd(const EventCollection<EventType>& other) {
    DoAddToEnd(other);
  }

  /**
   * Removes all events from this collection.
   */
  virtual void Clear() = 0;

  /**
   * Returns `false` if and only if this collection contains no events.
   */
  virtual bool HasEvents() const = 0;

  /**
   * Adds an event to this collection, or throws if the concrete collection
   * does not permit adding new events. Derived classes must implement this
   * method to add the specified event to the homogeneous event collection.
   */
  virtual void add_event(std::unique_ptr<EventType> event) = 0;

 protected:
  /**
   * Constructor only accessible by derived class.
   */
  EventCollection() = default;

  virtual void DoAddToEnd(const EventCollection<EventType>& other) = 0;
};

/**
 * A concrete class that holds all simultaneous homogeneous events for a
 * Diagram. For each subsystem in the corresponding Diagram, a derived
 * EventCollection instance is maintained internally, thus effectively holding
 * the same recursive tree structure as the corresponding Diagram.
 */
template <typename EventType>
class DiagramEventCollection final : public EventCollection<EventType> {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(DiagramEventCollection)

  /**
   * Note that this constructor only resizes the containers; it
   * does not allocate any derived EventCollection instances.
   *
   * @param num_subsystems Number of subsystems in the corresponding Diagram.
   */
  explicit DiagramEventCollection(int num_subsystems)
      : EventCollection<EventType>(),
        subevent_collection_(num_subsystems),
        owned_subevent_collection_(num_subsystems) {}

  /**
   * Throws if called, because no events should be added at the Diagram level.
   */
  void add_event(std::unique_ptr<EventType>) override {
    throw std::logic_error("DiagramEventCollection::add_event is not allowed");
  }

  /**
   * Returns the number of constituent EventCollection objects that correspond
   * to each subsystem in the Diagram.
   */
  int num_subsystems() const {
    return static_cast<int>(subevent_collection_.size());
  }

  /**
   * Transfers `subevent_collection` ownership to `this` and associates it
   * with the subsystem identified by `index`. Aborts if `index` is not in
   * the range [0, num_subsystems() - 1] or if `subevent_collection` is null.
   */
  void set_and_own_subevent_collection(
      int index,
      std::unique_ptr<EventCollection<EventType>> subevent_collection) {
    DRAKE_DEMAND(subevent_collection != nullptr);
    DRAKE_DEMAND(index >= 0 && index < num_subsystems());
    owned_subevent_collection_[index] = std::move(subevent_collection);
    subevent_collection_[index] = owned_subevent_collection_[index].get();
  }

  /**
   * Associate `subevent_collection` with subsystem identified by `index`.
   * Ownership of the object that `subevent_collection` is maintained
   * elsewhere, and its life span must be longer than this. Aborts if
   * `index` is not in the range [0, num_subsystems() - 1] or if
   * `subevent_collection` is null.
   */
  void set_subevent_collection(
      int index, EventCollection<EventType>* subevent_collection) {
    DRAKE_DEMAND(subevent_collection != nullptr);
    DRAKE_DEMAND(index >= 0 && index < num_subsystems());
    subevent_collection_[index] = subevent_collection;
  }

  /**
   * Returns a const pointer to subsystem's EventCollection at `index`.
   * Aborts if the 0-indexed `index` is greater than or equal to the number of
   * subsystems specified in this object's construction (see
   * DiagramEventCollection(int)) or if `index` is not in the range
   * [0, num_subsystems() - 1].
   */
  const EventCollection<EventType>& get_subevent_collection(int index) const {
    DRAKE_DEMAND(index >= 0 && index < num_subsystems());
    return *subevent_collection_[index];
  }

  /**
   * Returns a mutable pointer to subsystem's EventCollection at `index`.
   */
  EventCollection<EventType>& get_mutable_subevent_collection(int index) {
    DRAKE_DEMAND(index >= 0 && index < num_subsystems());
    return *subevent_collection_[index];
  }

  /**
   * Clears all subevent collections.
   */
  void Clear() override {
    for (EventCollection<EventType>* subevent : subevent_collection_) {
      subevent->Clear();
    }
  }

  /**
   * Returns `true` if and only if any of the subevent collections have any
   * events.
   */
  bool HasEvents() const override {
    for (const EventCollection<EventType>* subevent : subevent_collection_) {
      if (subevent->HasEvents()) return true;
    }
    return false;
  }

 protected:
  /**
   * Goes through each subevent collection of `this` and adds the corresponding
   * one in `other_collection` to the subevent collection in `this`. Aborts if
   * `this` does not have the same number of subevent collections as
   * `other_collection`. In addition, this method assumes that `this` and
   * `other_collection` have the exact same topology (i.e. both are created for
   * the same Diagram.)
   * @throws std::bad_cast if `other_collection` is not an instance of
   * DiagramEventCollection.
   */
  void DoAddToEnd(
      const EventCollection<EventType>& other_collection) override {
    const DiagramEventCollection<EventType>& other =
        dynamic_cast<const DiagramEventCollection<EventType>&>(
            other_collection);
    DRAKE_DEMAND(num_subsystems() == other.num_subsystems());

    for (int i = 0; i < num_subsystems(); i++) {
      subevent_collection_[i]->AddToEnd(other.get_subevent_collection(i));
    }
  }

 private:
  std::vector<EventCollection<EventType>*> subevent_collection_;
  std::vector<std::unique_ptr<EventCollection<EventType>>>
      owned_subevent_collection_;
};

/**
 * A concrete class that holds all simultaneous homogeneous events for a
 * LeafSystem.
 */
template <typename EventType>
class LeafEventCollection final : public EventCollection<EventType> {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(LeafEventCollection)

  /**
   * Constructor.
   */
  LeafEventCollection() = default;

  /**
   * Static method that generates a LeafEventCollection with exactly
   * one event with no optional attribute, data or callback, and trigger type
   * kForced.
   */
  static std::unique_ptr<LeafEventCollection<EventType>>
  MakeForcedEventCollection() {
    auto ret = std::make_unique<LeafEventCollection<EventType>>();
    auto event = std::make_unique<EventType>(EventType::TriggerType::kForced);
    ret->add_event(std::move(event));
    return ret;
  }

  /**
   * Returns a const reference to the vector of const pointers to all of the
   * events.
   */
  // TODO(siyuan): provide an iterator instead.
  const std::vector<const EventType*>& get_events() const { return events_; }

  /**
   * Add `event` to the existing collection. Ownership of `event` is
   * transferred. Aborts if event is null.
   */
  void add_event(std::unique_ptr<EventType> event) override {
    DRAKE_DEMAND(event != nullptr);
    owned_events_.push_back(std::move(event));
    events_.push_back(owned_events_.back().get());
  }

  /**
   * Returns `true` if and only if this collection is nonempty.
   */
  bool HasEvents() const override { return !events_.empty(); }

  /**
   * Removes all events from this collection.
   */
  void Clear() override {
    owned_events_.clear();
    events_.clear();
  }

 protected:
  /**
   * All events in `other_collection` are concatanated to this.
   *
   * Here is an example. Suppose this collection stores the following events:
   * <pre>
   *   EventType: {event1, event2, event3}
   * </pre>
   * `other_collection` has:
   * <pre>
   *   EventType: {event4}
   * </pre>
   * After calling DoAddToEnd(other_collection), `this` stores:
   * <pre>
   *   EventType: {event1, event2, event3, event4}
   * </pre>
   *
   * @throws std::bad_cast if `other_collection` is not an instance of
   * LeafEventCollection.
   */
  void DoAddToEnd(const EventCollection<EventType>& other_collection) override {
    const LeafEventCollection<EventType>& other =
        dynamic_cast<const LeafEventCollection<EventType>&>(other_collection);

    const std::vector<const EventType*>& other_events = other.get_events();
    for (const EventType* other_event : other_events) {
      this->add_event(static_pointer_cast<EventType>(other_event->Clone()));
    }
  }

 private:
  // Owned event unique pointers.
  std::vector<std::unique_ptr<EventType>> owned_events_;

  // Points to the corresponding unique pointers. This is primarily used for
  // get_events().
  std::vector<const EventType*> events_;
};

/**
 * This class bundles an instance of each EventCollection<EventType> into one
 * object that stores the heterogeneous collection. This is intended to hold
 * heterogeneous events returned by methods like System::CalcNextUpdateTime.
 * <pre>
 * CompositeEventCollection<T> = {
 *   EventCollection<PublishEvent<T>>,
 *   EventCollection<DiscreteUpdateEvent<T>>,
 *   EventCollection<UnrestrictedUpdate<T>>}
 * </pre>
 * There are two concrete derived classes: LeafCompositeEventCollection and
 * DiagramCompositeEventCollection. Adding new events to the collection is
 * only allowed for LeafCompositeEventCollection.
 *
 * @tparam T needs to be compatible with Eigen Scalar type.
 */
template <typename T>
class CompositeEventCollection {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(CompositeEventCollection)

  virtual ~CompositeEventCollection() {}

  /**
   * Clears all the events.
   */
  void Clear() {
    publish_events_->Clear();
    discrete_update_events_->Clear();
    unrestricted_update_events_->Clear();
  }

  /**
   * Returns `true` if and only if this collection contains any events.
   */
  bool HasEvents() const {
    return (publish_events_->HasEvents() ||
            discrete_update_events_->HasEvents() ||
            unrestricted_update_events_->HasEvents());
  }

  /**
   * Returns `true` if and only if this collection contains one or more
   * publish events.
   */
  bool HasPublishEvents() const { return publish_events_->HasEvents(); }

  /**
   * Returns `true` if and only if this collection contains one or more
   * discrete update events.
   */
  bool HasDiscreteUpdateEvents() const {
    return discrete_update_events_->HasEvents();
  }

  /**
   * Returns `true` if and only if this collection contains one or more
   * unrestricted update events.
   */
  bool HasUnrestrictedUpdateEvents() const {
    return unrestricted_update_events_->HasEvents();
  }

  /**
   * Assuming the internal publish event collection is an instance of
   * LeafEventCollection, adds the publish event `event` (ownership is also
   * transferred) to it.
   * @throws std::bad_cast if the assumption is incorrect.
   */
  void add_publish_event(std::unique_ptr<PublishEvent<T>> event) {
    DRAKE_DEMAND(event != nullptr);
    auto& events = dynamic_cast<LeafEventCollection<PublishEvent<T>>&>(
        this->get_mutable_publish_events());
    events.add_event(std::move(event));
  }

  /**
   * Assuming the internal discrete update event collection is an instance of
   * LeafEventCollection, adds the discrete update event `event` (ownership is
   * also transferred) to it.
   * @throws std::bad_cast if the assumption is incorrect.
   */
  void add_discrete_update_event(
      std::unique_ptr<DiscreteUpdateEvent<T>> event) {
    DRAKE_DEMAND(event != nullptr);
    auto& events = dynamic_cast<LeafEventCollection<DiscreteUpdateEvent<T>>&>(
        this->get_mutable_discrete_update_events());
    events.add_event(std::move(event));
  }

  /**
   * Assuming the internal unrestricted update event collection is an instance
   * of LeafEventCollection, adds the unrestricted update event `event`
   * (ownership is also transferred) to it.
   * @throws std::bad_cast if the assumption is incorrect.
   */
  void add_unrestricted_update_event(
      std::unique_ptr<UnrestrictedUpdateEvent<T>> event) {
    DRAKE_DEMAND(event != nullptr);
    auto& events =
        dynamic_cast<LeafEventCollection<UnrestrictedUpdateEvent<T>>&>(
            this->get_mutable_unrestricted_update_events());
    events.add_event(std::move(event));
  }

  /**
   * Adds the contained homogeneous event collections (e.g.,
   * EventCollection<PublishEvent<T>>, EventCollection<DiscreteUpdateEvent<T>>,
   * etc.) from `other` to the end of `this`.
   */
  void AddToEnd(const CompositeEventCollection<T>& other) {
    publish_events_->AddToEnd(other.get_publish_events());
    discrete_update_events_->AddToEnd(other.get_discrete_update_events());
    unrestricted_update_events_->AddToEnd(
        other.get_unrestricted_update_events());
  }

  /**
   * Copies the collections of homogeneous events from `other` to `this`.
   */
  void SetFrom(const CompositeEventCollection<T>& other) {
    publish_events_->SetFrom(other.get_publish_events());
    discrete_update_events_->SetFrom(other.get_discrete_update_events());
    unrestricted_update_events_->SetFrom(
        other.get_unrestricted_update_events());
  }

  /**
   * Returns a const reference to the collection of publish events.
   */
  const EventCollection<PublishEvent<T>>& get_publish_events() const {
    return *publish_events_;
  }

  /**
   * Returns a const reference to the collection of discrete update events.
   */
  const EventCollection<DiscreteUpdateEvent<T>>& get_discrete_update_events()
      const {
    return *discrete_update_events_;
  }

  /**
   * Returns a const reference to the collection of unrestricted update events.
   */
  const EventCollection<UnrestrictedUpdateEvent<T>>&
  get_unrestricted_update_events() const {
    return *unrestricted_update_events_;
  }

  /**
   * Returns a mutable reference to the collection of publish events
   */
  EventCollection<PublishEvent<T>>& get_mutable_publish_events() const {
    return *publish_events_;
  }

  /**
   * Returns a mutable reference to the collection of discrete update events.
   */
  EventCollection<DiscreteUpdateEvent<T>>& get_mutable_discrete_update_events()
      const {
    return *discrete_update_events_;
  }

  /**
   * Returns a mutable reference to the collection of unrestricted update
   * events.
   */
  EventCollection<UnrestrictedUpdateEvent<T>>&
  get_mutable_unrestricted_update_events() const {
    return *unrestricted_update_events_;
  }

 protected:
  /**
   * Takes ownership of `pub`, `discrete` and `unrestricted`. Aborts if any
   * of these are null.
   */
  CompositeEventCollection(
      std::unique_ptr<EventCollection<PublishEvent<T>>> pub,
      std::unique_ptr<EventCollection<DiscreteUpdateEvent<T>>> discrete,
      std::unique_ptr<EventCollection<UnrestrictedUpdateEvent<T>>> unrestricted)
      : publish_events_(std::move(pub)),
        discrete_update_events_(std::move(discrete)),
        unrestricted_update_events_(std::move(unrestricted)) {
    DRAKE_DEMAND(publish_events_ != nullptr);
    DRAKE_DEMAND(discrete_update_events_ != nullptr);
    DRAKE_DEMAND(unrestricted_update_events_ != nullptr);
  }

 private:
  std::unique_ptr<EventCollection<PublishEvent<T>>> publish_events_{nullptr};
  std::unique_ptr<EventCollection<DiscreteUpdateEvent<T>>>
      discrete_update_events_{nullptr};
  std::unique_ptr<EventCollection<UnrestrictedUpdateEvent<T>>>
      unrestricted_update_events_{nullptr};
};

/**
 * A CompositeEventCollection for a LeafSystem. i.e.
 * <pre>
 *   PublishEvent<T>: {event1i, ...}
 *   DiscreteUpdateEvent<T>: {event2i, ...}
 *   UnrestrictedUpdateEvent<T>: {event3i, ...}
 * </pre>
 */
template <typename T>
class LeafCompositeEventCollection final : public CompositeEventCollection<T> {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(LeafCompositeEventCollection)

  LeafCompositeEventCollection()
      : CompositeEventCollection<T>(
            std::make_unique<LeafEventCollection<PublishEvent<T>>>(),
            std::make_unique<LeafEventCollection<DiscreteUpdateEvent<T>>>(),
            std::make_unique<
                LeafEventCollection<UnrestrictedUpdateEvent<T>>>()) {}

  /**
   * Returns a const reference to the collection of publish events.
   */
  const LeafEventCollection<PublishEvent<T>>& get_publish_events() const {
    return dynamic_cast<const LeafEventCollection<PublishEvent<T>>&>(
        CompositeEventCollection<T>::get_publish_events());
  }

  /**
   * Returns a const reference to the collection of discrete update events.
   */
  const LeafEventCollection<DiscreteUpdateEvent<T>>&
  get_discrete_update_events() const {
    return dynamic_cast<const LeafEventCollection<DiscreteUpdateEvent<T>>&>(
        CompositeEventCollection<T>::get_discrete_update_events());
  }

  /**
   * Returns a const reference to the collection of unrestricted update events.
   */
  const LeafEventCollection<UnrestrictedUpdateEvent<T>>&
  get_unrestricted_update_events() const {
    return dynamic_cast<const LeafEventCollection<UnrestrictedUpdateEvent<T>>&>(
        CompositeEventCollection<T>::get_unrestricted_update_events());
  }
};

/**
 * CompositeEventCollection for a Diagram.
 */
template <typename T>
class DiagramCompositeEventCollection final
    : public CompositeEventCollection<T> {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(DiagramCompositeEventCollection)

  /**
   * Allocated CompositeEventCollection for all constituent subsystems are
   * passed in `subevents`, for which ownership is also transferred to `this`.
   */
  explicit DiagramCompositeEventCollection(
      std::vector<std::unique_ptr<CompositeEventCollection<T>>> subevents)
      : CompositeEventCollection<T>(
            std::make_unique<DiagramEventCollection<PublishEvent<T>>>(
                subevents.size()),
            std::make_unique<DiagramEventCollection<DiscreteUpdateEvent<T>>>(
                subevents.size()),
            std::make_unique<
                DiagramEventCollection<UnrestrictedUpdateEvent<T>>>(
                subevents.size())),
        owned_subevent_collection_(std::move(subevents)) {
    size_t num_subsystems = owned_subevent_collection_.size();

    for (size_t i = 0; i < num_subsystems; ++i) {
      DiagramEventCollection<PublishEvent<T>>& sub_publish =
          dynamic_cast<DiagramEventCollection<PublishEvent<T>>&>(
              this->get_mutable_publish_events());
      // Sets sub_publish's i'th subsystem's EventCollection<PublishEvent>
      // pointer to owned_subevent_collection_[i].get_mutable_publish_events().
      // So that sub_publish has the same pointer structure, but does not
      // duplicate actual data.
      sub_publish.set_subevent_collection(
          i, &(owned_subevent_collection_[i]->get_mutable_publish_events()));

      DiagramEventCollection<DiscreteUpdateEvent<T>>& sub_discrete_update =
          dynamic_cast<DiagramEventCollection<DiscreteUpdateEvent<T>>&>(
              this->get_mutable_discrete_update_events());
      sub_discrete_update.set_subevent_collection(
          i, &(owned_subevent_collection_[i]
                   ->get_mutable_discrete_update_events()));

      DiagramEventCollection<UnrestrictedUpdateEvent<T>>&
          sub_unrestricted_update =
              dynamic_cast<DiagramEventCollection<UnrestrictedUpdateEvent<T>>&>(
                  this->get_mutable_unrestricted_update_events());
      sub_unrestricted_update.set_subevent_collection(
          i, &(owned_subevent_collection_[i]
                   ->get_mutable_unrestricted_update_events()));
    }
  }

  /**
   * Returns the number of subsystems for which this object contains event
   * collections.
   */
  int num_subsystems() const {
    return static_cast<int>(owned_subevent_collection_.size());
  }

  // Gets a mutable pointer to the CompositeEventCollection specified for the
  // given subsystem. Aborts if the 0-index `index` is greater than or equal
  // to the number of subsystems or if `index` is negative.
  CompositeEventCollection<T>& get_mutable_subevent_collection(int index) {
    DRAKE_DEMAND(index >= 0 && index < num_subsystems());
    return *owned_subevent_collection_[index].get();
  }

  // Gets a const reference to the CompositeEventCollection specified for
  // the given subsystem. Aborts if the 0-index `index` is greater than or
  // equal to the number of subsystems or if `index` is negative.
  const CompositeEventCollection<T>& get_subevent_collection(int index) const {
    DRAKE_DEMAND(index >= 0 && index < num_subsystems());
    return *owned_subevent_collection_[index].get();
  }

 private:
  std::vector<std::unique_ptr<CompositeEventCollection<T>>>
      owned_subevent_collection_;
};

}  // namespace systems
}  // namespace drake

DRAKE_DECLARE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS(
    class ::drake::systems::CompositeEventCollection)

DRAKE_DECLARE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS(
    class ::drake::systems::LeafCompositeEventCollection)

DRAKE_DECLARE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS(
    class ::drake::systems::DiagramCompositeEventCollection)
