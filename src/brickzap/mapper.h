// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QtCore/QString>

#include "bricklink/lot.h"
#include "brickzap/global.h"

QT_FORWARD_DECLARE_CLASS(QJsonArray)
QT_FORWARD_DECLARE_CLASS(QJsonObject)

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

double amountToPrice(qint64 amount);
qint64 priceToAmount(double price);
QJsonObject money(double price, const QString &currencyCode);

// Returns nullptr if the item cannot be resolved in the BrickLink catalog.
BrickLink::Lot *lotFromListing(const QJsonObject &listing, QString *currencyCode = nullptr);
BrickLink::Lot *lotFromOrderItem(const QJsonObject &orderItem, QString *currencyCode = nullptr);

QJsonObject importItemFromLot(const BrickLink::Lot *lot, const QString &currencyCode);

} // namespace BrickZap::Mapper
