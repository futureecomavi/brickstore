// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#include <cmath>

#include <QJsonArray>
#include <QJsonObject>

#include "bricklink/color.h"
#include "bricklink/core.h"
#include "bricklink/item.h"
#include "bricklink/itemtype.h"
#include "brickzap/mapper.h"

using namespace BrickLink;

namespace {

char itemTypeIdFromProductType(const QString &productType)
{
    static const QHash<QString, char> map = {
        { u"part"_qs,          'P' },
        { u"set"_qs,           'S' },
        { u"minifigure"_qs,    'M' },
        { u"minifig"_qs,       'M' },
        { u"gear"_qs,          'G' },
        { u"book"_qs,          'B' },
        { u"instruction"_qs,   'I' },
        { u"original_box"_qs,  'O' },
        { u"catalog"_qs,       'C' },
    };
    return map.value(productType.toLower(), char(0));
}

QString productTypeFromItemTypeId(char itemTypeId)
{
    switch (itemTypeId) {
    case 'P': return u"part"_qs;
    case 'S': return u"set"_qs;
    case 'M': return u"minifigure"_qs;
    case 'G': return u"gear"_qs;
    case 'B': return u"book"_qs;
    case 'I': return u"instruction"_qs;
    case 'O': return u"original_box"_qs;
    case 'C': return u"catalog"_qs;
    default:  return { };
    }
}

bool isBrickLinkSource(const QJsonObject &obj, const QString &key)
{
    return obj.value(key).toString().compare(u"bricklink"_qs, Qt::CaseInsensitive) == 0;
}

// The BrickLink item id, but only if the listing is actually addressed in the BrickLink
// namespace: anything else cannot be resolved against BrickStore's catalog.
QByteArray brickLinkItemId(const QJsonObject &obj)
{
    // `external_sku` is the verbatim source identifier and is also present on order lines,
    // where the catalog SKU fields are not part of the listing snapshot
    if (isBrickLinkSource(obj, u"external_source"_qs)) {
        const QString sku = obj.value(u"external_sku"_qs).toString();
        if (!sku.isEmpty())
            return sku.toLatin1();
    }
    if (isBrickLinkSource(obj, u"catalog_sku_source"_qs))
        return obj.value(u"catalog_sku"_qs).toString().toLatin1();
    return { };
}

const Item *resolveItem(const QJsonObject &obj)
{
    const QByteArray id = brickLinkItemId(obj);
    if (id.isEmpty())
        return nullptr;

    const char itemTypeId = itemTypeIdFromProductType(obj.value(u"product_type"_qs).toString());
    return itemTypeId ? core()->item(itemTypeId, id)
                      : core()->item(std::string("PSMGBIOC"), id);
}

const Color *resolveColor(const QJsonObject &obj)
{
    if (isBrickLinkSource(obj, u"external_source"_qs)) {
        bool isNumeric = false;
        const uint colorId = obj.value(u"external_color_id"_qs).toString().toUInt(&isNumeric);
        if (isNumeric) {
            if (const auto *color = core()->color(colorId))
                return color;
        }
    }
    // BrickZap's own color codes are not numeric, so fall back to the color name
    const QString name = obj.value(u"color_name"_qs).toString();
    if (!name.isEmpty()) {
        if (const auto *color = core()->colorFromName(name))
            return color;
    }
    return core()->color(0);
}

Condition conditionFromString(const QString &condition)
{
    return (condition.compare(u"used"_qs, Qt::CaseInsensitive) == 0) ? Condition::Used
                                                                    : Condition::New;
}

SubCondition subConditionFromCompleteness(const QString &completeness)
{
    if (completeness.compare(u"sealed"_qs, Qt::CaseInsensitive) == 0)
        return SubCondition::Sealed;
    else if (completeness.compare(u"complete"_qs, Qt::CaseInsensitive) == 0)
        return SubCondition::Complete;
    else if (completeness.compare(u"incomplete"_qs, Qt::CaseInsensitive) == 0)
        return SubCondition::Incomplete;
    return SubCondition::None;
}

QString completenessFromSubCondition(SubCondition subCondition)
{
    switch (subCondition) {
    case SubCondition::Sealed:     return u"sealed"_qs;
    case SubCondition::Complete:   return u"complete"_qs;
    case SubCondition::Incomplete: return u"incomplete"_qs;
    default:                       return { };
    }
}

Stockroom stockroomFromString(const QString &stockroom)
{
    if (stockroom == u"A")
        return Stockroom::A;
    else if (stockroom == u"B")
        return Stockroom::B;
    else if (stockroom == u"C")
        return Stockroom::C;
    return Stockroom::None;
}

QString stockroomToString(Stockroom stockroom)
{
    switch (stockroom) {
    case Stockroom::A: return u"A"_qs;
    case Stockroom::B: return u"B"_qs;
    case Stockroom::C: return u"C"_qs;
    default:           return { };
    }
}

double priceFromMoney(const QJsonObject &obj, const QString &key, QString *currencyCode)
{
    const auto money = obj.value(key).toObject();
    if (money.isEmpty())
        return 0;

    if (currencyCode) {
        const QString currency = money.value(u"currency"_qs).toString();
        if (!currency.isEmpty() && currencyCode->isEmpty())
            *currencyCode = currency;
    }
    return BrickZap::Mapper::amountToPrice(qint64(money.value(u"amount"_qs).toDouble()));
}

} // namespace


namespace BrickZap::Mapper {

double amountToPrice(qint64 amount)
{
    return double(amount) / moneyFactor;
}

qint64 priceToAmount(double price)
{
    return qint64(std::llround(price * moneyFactor));
}

QJsonObject money(double price, const QString &currencyCode)
{
    return QJsonObject {
        { u"amount"_qs, priceToAmount(price) },
        { u"currency"_qs, currencyCode },
    };
}

Lot *lotFromListing(const QJsonObject &listing, QString *currencyCode)
{
    const auto *item = resolveItem(listing);
    if (!item)
        return nullptr;

    auto *lot = new Lot(item, resolveColor(listing));

    lot->setQuantity(int(listing.value(u"inventory"_qs).toObject()
                             .value(u"quantity"_qs).toDouble()));
    lot->setPrice(priceFromMoney(listing, u"price"_qs, currencyCode));
    lot->setCondition(conditionFromString(listing.value(u"condition"_qs).toString()));

    if (item->itemTypeId() == 'S') {
        lot->setSubCondition(subConditionFromCompleteness(
            listing.value(u"completeness"_qs).toString()));
    }

    lot->setComments(listing.value(u"description"_qs).toString());
    lot->setRemarks(listing.value(u"condition_notes"_qs).toString());
    lot->setStockroom(stockroomFromString(listing.value(u"stockroom"_qs).toString()));

    if (const int bulk = listing.value(u"bulk_quantity"_qs).toInt(); bulk > 1)
        lot->setBulkQuantity(bulk);

    const auto tiers = listing.value(u"tiers"_qs).toArray();
    for (int i = 0; (i < tiers.size()) && (i < 3); ++i) {
        const auto tier = tiers.at(i).toObject();
        const int quantity = int(tier.value(u"quantity"_qs).toDouble());
        const double price = priceFromMoney(tier, u"price"_qs, nullptr);

        if ((quantity > 0) && !qFuzzyIsNull(price)) {
            lot->setTierQuantity(i, quantity);
            lot->setTierPrice(i, price);
        }
    }

    // BrickLink lot ids are the shared identity between BrickStore and BrickZap listings
    if (isBrickLinkSource(listing, u"external_source"_qs)) {
        bool isNumeric = false;
        const uint lotId = listing.value(u"external_lot_id"_qs).toString().toUInt(&isNumeric);
        if (isNumeric)
            lot->setLotId(lotId);
    }
    return lot;
}

Lot *lotFromOrderItem(const QJsonObject &orderItem, QString *currencyCode)
{
    // order lines carry a snapshot of the listing, so the same fields apply
    auto *lot = lotFromListing(orderItem, currencyCode);
    if (!lot)
        return nullptr;

    lot->setQuantity(int(orderItem.value(u"quantity"_qs).toDouble()));

    const double realPrice = priceFromMoney(orderItem, u"real_price"_qs, currencyCode);
    if (!qFuzzyIsNull(realPrice))
        lot->setPrice(realPrice);

    return lot;
}

QJsonObject importItemFromLot(const Lot *lot, const QString &currencyCode)
{
    QJsonObject item {
        { u"catalog_sku"_qs, QString::fromLatin1(lot->itemId()) },
        { u"catalog_sku_source"_qs, u"bricklink"_qs },
        { u"inventory"_qs, QJsonObject { { u"quantity"_qs, lot->quantity() } } },
        { u"price"_qs, money(lot->price(), currencyCode) },
        { u"condition"_qs, (lot->condition() == Condition::Used) ? u"used"_qs : u"new"_qs },
    };

    if (const auto productType = productTypeFromItemTypeId(lot->itemTypeId());
        !productType.isEmpty()) {
        item[u"product_type"_qs] = productType;
    }
    if (lot->colorId())
        item[u"color_code"_qs] = QString::number(lot->colorId());

    if (const auto completeness = completenessFromSubCondition(lot->subCondition());
        !completeness.isEmpty()) {
        item[u"completeness"_qs] = completeness;
    }
    if (!lot->comments().isEmpty())
        item[u"description"_qs] = lot->comments();
    if (!lot->remarks().isEmpty())
        item[u"condition_notes"_qs] = lot->remarks();

    if (const auto stockroom = stockroomToString(lot->stockroom()); !stockroom.isEmpty()) {
        item[u"stockroom"_qs] = stockroom;
        item[u"stockroom_reason"_qs] = u"sync_imported"_qs;
    }
    if (lot->bulkQuantity() > 1)
        item[u"bulk_quantity"_qs] = lot->bulkQuantity();

    QJsonArray tiers;
    for (int i = 0; i < 3; ++i) {
        if ((lot->tierQuantity(i) > 0) && !qFuzzyIsNull(lot->tierPrice(i))) {
            tiers.append(QJsonObject {
                { u"quantity"_qs, lot->tierQuantity(i) },
                { u"type"_qs, u"fixed"_qs },
                { u"price"_qs, money(lot->tierPrice(i), currencyCode) },
            });
        }
    }
    if (!tiers.isEmpty())
        item[u"tiers"_qs] = tiers;

    if (lot->hasCustomWeight()) {
        item[u"dimensions"_qs] = QJsonObject {
            { u"weight"_qs, lot->weight() },
            { u"weight_unit"_qs, u"g"_qs },
        };
    }

    // external lot tracking lets a re-upload merge into the listings created by an earlier
    // upload (or by BrickZap's own BrickLink sync) instead of duplicating them
    if (lot->lotId()) {
        item[u"external_source"_qs] = u"bricklink"_qs;
        item[u"external_lot_id"_qs] = QString::number(lot->lotId());
        item[u"external_sku"_qs] = QString::fromLatin1(lot->itemId());
        if (lot->colorId())
            item[u"external_color_id"_qs] = QString::number(lot->colorId());
    }
    return item;
}

} // namespace BrickZap::Mapper
