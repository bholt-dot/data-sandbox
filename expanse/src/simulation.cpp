#include "expanse/simulation.hpp"

#include <type_traits>

namespace expanse {

void post(World& world, MessageKind kind, std::string text, bool urgent) {
    world.messages.push_back(Message{world.now(), kind, urgent, std::move(text)});
}

void dispatch(const Content& content, World& world, sim::Scheduler<Event>& scheduler, const Event& event) {
    std::visit(
        [&](const auto& e) {
            using E = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<E, DailyTick>) {
                systems::daily_economy(content, world);
                systems::daily_finance(content, world);
                systems::daily_crew(content, world);
                systems::daily_contracts(content, world);
            } else if constexpr (std::is_same_v<E, WeeklyTick>) {
                systems::weekly_crew(content, world);
            } else if constexpr (std::is_same_v<E, LoanPaymentDue>) {
                systems::loan_payment_due(content, world, scheduler, e.loan);
            } else if constexpr (std::is_same_v<E, ShipArrives>) {
                systems::ship_arrives(content, world, scheduler, e.ship);
            } else {
                static_assert(!sizeof(E), "unhandled event type");
            }
        },
        event);
}

AdvanceReport advance_to(const Content& content, World& world, sim::Time until, bool stop_on_urgent) {
    AdvanceReport report;
    report.first_new_message = world.messages.size();
    std::size_t checked = world.messages.size();

    auto handler = [&](sim::Scheduler<Event>& s, const sim::Occurrence<Event>& occ) {
        dispatch(content, world, s, occ.payload);
    };
    auto stop = [&](const sim::Occurrence<Event>& occ) {
        if (!stop_on_urgent) {
            return false;
        }
        bool urgent = (occ.flags & player_event) != 0;
        for (; checked < world.messages.size(); ++checked) {
            urgent = urgent || world.messages[checked].urgent;
        }
        return urgent;
    };
    const auto result = world.scheduler.advance_until(until, handler, stop);
    report.occurrences = result.events_fired + result.periodic_fired;
    report.stopped_early = result.stopped_on.has_value();
    return report;
}

} // namespace expanse
