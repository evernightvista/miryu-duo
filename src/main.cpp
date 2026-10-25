/*
    SPDX-FileCopyrightText: 2026 Miryu Gaming <dev@miryugaming.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "miryuduo.h"

namespace KWin
{

KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(MiryuDuoEffect,
                                      "metadata.json",
                                      return MiryuDuoEffect::supported();,
                                      return true;)

} // namespace KWin

#include "main.moc"
