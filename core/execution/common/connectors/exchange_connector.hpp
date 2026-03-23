#pragma once


#include "../../../common/types.hpp"
#include <functional>

namespace trading {
    class ExchangeConnector {
    public:
        virtual ~ExchangeConnector() = default;

        virtual Exchange exchange_id() const = 0;

        virtual bool is_connected() const = 0;

        virtual ConnectorResult connect() = 0;

        virtual void disconnect() = 0;

        virtual ConnectorResult submit_order(const Order &order) = 0;

        virtual ConnectorResult cancel_order(uint64_t client_order_id) = 0;

        virtual ConnectorResult replace_order(uint64_t client_order_id, const Order &replacement) = 0;

        virtual ConnectorResult query_order(uint64_t client_order_id, FillUpdate &status) = 0;

        virtual ConnectorResult cancel_all(const char *symbol) = 0;

        virtual ConnectorResult reconcile() = 0;

        std::function<void(const FillUpdate &)> on_fill;

    protected:
        void emit_fill(const FillUpdate &u) {
            if (on_fill)
                on_fill(u);
        }
    };
}
