// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <limits>

#include <QtCore/QString>

#include "bricklink/lot.h"
#include "brickzap/global.h"

QT_FORWARD_DECLARE_CLASS(QJsonArray)
QT_FORWARD_DECLARE_CLASS(QJsonObject)
QT_FORWARD_DECLARE_CLASS(QJsonValue)

// Translates between BrickZap listings/order lines and BrickStore lots.
//
// BrickZap has no catalog of its own in BrickStore: all items are addressed in the BrickLink
// namespace (`catalog_sku_source: bricklink`), which is exactly what the BrickLink database
// already provides. BrickLink lot ids are round-tripped through the external lot tracking
// fields (`external_source` / `external_lot_id`), so re-uploading a store merges into the
// existing listings instead of duplicating them.

namespace BrickZap::Mapper {

// BrickZap money is an integer amount in minor units, matching the server-side
// `round(price * 100)` conversion used by its own CSV/BSX importers.
constexpr double moneyFactor = 100;
// nothing sane is anywhere near this: it only exists to keep a hostile or broken response
// from producing a value that cannot be represented
constexpr double maxAmount = 1e15;

// clamping accessors for untrusted JSON numbers
int toInt(const QJsonValue &value, int min = 0,
          int max = (std::numeric_limits<int>::max)());
double amountToPrice(const QJsonValue &amount);
double priceFromMoney(const QJsonObject &obj, const QString &key,
                      QString *currencyCode = nullptr);

qint64 priceToAmount(double price);
QJsonObject money(double price, const QString &currencyCode);

// Returns nullptr if the item cannot be resolved in the BrickLink catalog.
BrickLink::Lot *lotFromListing(const QJsonObject &listing, QString *currencyCode = nullptr);
BrickLink::Lot *lotFromOrderItem(const QJsonObject &orderItem, QString *currencyCode = nullptr);

QJsonObject importItemFromLot(const BrickLink::Lot *lot, const QString &currencyCode);

} // namespace BrickZap::Mapper
