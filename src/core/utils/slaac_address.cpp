/*
 *  Copyright (c) 2016, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements the Thread IPv6 global addresses configuration utilities.
 */

#include "slaac_address.hpp"

#include "utils/wrap_string.h"

#include "common/code_utils.hpp"
#include "common/instance.hpp"
#include "common/logging.hpp"
#include "common/owner-locator.hpp"
#include "common/random.hpp"
#include "common/settings.hpp"
#include "crypto/sha256.hpp"
#include "net/ip6_address.hpp"

#if OPENTHREAD_CONFIG_ENABLE_SLAAC

namespace ot {
namespace Utils {

Slaac::Slaac(Instance &aInstance)
    : InstanceLocator(aInstance)
    , mEnabled(true)
    , mFilter(NULL)
    , mNotifierCallback(aInstance, &Slaac::HandleStateChanged, this)
{
    memset(mAddresses, 0, sizeof(mAddresses));
}

void Slaac::Enable(void)
{
    VerifyOrExit(!mEnabled);

    otLogInfoUtil("SLAAC:: Enabling");
    mEnabled = true;
    Update(kModeAdd);

exit:
    return;
}

void Slaac::Disable(void)
{
    VerifyOrExit(mEnabled);

    otLogInfoUtil("SLAAC:: Disabling");
    mEnabled = false;
    Update(kModeRemove);

exit:
    return;
}

void Slaac::SetFilter(otIp6SlaacPrefixFilter aFilter)
{
    VerifyOrExit(aFilter != mFilter);

    mFilter = aFilter;
    otLogInfoUtil("SLAAC: Filter %s", (mFilter != NULL) ? "updated" : "disabled");

    VerifyOrExit(mEnabled);
    Update(kModeAdd | kModeRemove);

exit:
    return;
}

bool Slaac::ShouldFilter(const otIp6Prefix &aPrefix) const
{
    return (mFilter != NULL) && mFilter(&GetInstance(), &aPrefix);
}

void Slaac::HandleStateChanged(Notifier::Callback &aCallback, otChangedFlags aFlags)
{
    aCallback.GetOwner<Slaac>().HandleStateChanged(aFlags);
}

void Slaac::HandleStateChanged(otChangedFlags aFlags)
{
    UpdateMode mode = kModeNone;

    VerifyOrExit(mEnabled);

    if (aFlags & OT_CHANGED_THREAD_NETDATA)
    {
        mode |= kModeAdd | kModeRemove;
    }

    if (aFlags & OT_CHANGED_IP6_ADDRESS_REMOVED)
    {
        // When an IPv6 address is removed, we ensure to check if a SLAAC address
        // needs to be added (replacing the removed address).
        //
        // Note that if an address matching a newly added on-mesh prefix (with
        // SLAAC flag) is already present (e.g., user previously added an address
        // with same prefix), the SLAAC module will not add a SLAAC address with same
        // prefix. So on IPv6 address removal event, we check if SLAAC module need
        // to add any addresses.

        mode |= kModeAdd;
    }

    if (mode != kModeNone)
    {
        Update(mode);
    }

exit:
    return;
}

void Slaac::Update(UpdateMode aMode)
{
    ThreadNetif &             netif       = GetNetif();
    NetworkData::Leader &     networkData = GetInstance().Get<NetworkData::Leader>();
    otNetworkDataIterator     iterator;
    otBorderRouterConfig      config;
    Ip6::NetifUnicastAddress *slaacAddr;
    bool                      found;

    if (aMode & kModeRemove)
    {
        // If enabled, remove any SLAAC addresses with no matching on-mesh prefix,
        // otherwise (when disabled) remove all previously added SLAAC addresses.

        for (slaacAddr = &mAddresses[0]; slaacAddr < &mAddresses[OT_ARRAY_LENGTH(mAddresses)]; slaacAddr++)
        {
            if (!slaacAddr->mValid)
            {
                continue;
            }

            found = false;

            if (mEnabled)
            {
                iterator = OT_NETWORK_DATA_ITERATOR_INIT;

                while (networkData.GetNextOnMeshPrefix(&iterator, &config) == OT_ERROR_NONE)
                {
                    otIp6Prefix &prefix = config.mPrefix;

                    if (config.mSlaac && !ShouldFilter(prefix) && (prefix.mLength == slaacAddr->mPrefixLength) &&
                        (slaacAddr->GetAddress().PrefixMatch(prefix.mPrefix) >= prefix.mLength))
                    {
                        found = true;
                        break;
                    }
                }
            }

            if (!found)
            {
                otLogInfoUtil("SLAAC: Removing address %s", slaacAddr->GetAddress().ToString().AsCString());

                netif.RemoveUnicastAddress(*slaacAddr);
                slaacAddr->mValid = false;
            }
        }
    }

    if ((aMode & kModeAdd) && mEnabled)
    {
        // Generate and add SLAAC addresses for any newly added on-mesh prefixes.

        iterator = OT_NETWORK_DATA_ITERATOR_INIT;

        while (networkData.GetNextOnMeshPrefix(&iterator, &config) == OT_ERROR_NONE)
        {
            otIp6Prefix &prefix = config.mPrefix;

            if (!config.mSlaac || ShouldFilter(prefix))
            {
                continue;
            }

            found = false;

            for (const Ip6::NetifUnicastAddress *netifAddr = netif.GetUnicastAddresses(); netifAddr != NULL;
                 netifAddr                                 = netifAddr->GetNext())
            {
                if ((netifAddr->mPrefixLength == prefix.mLength) &&
                    (netifAddr->GetAddress().PrefixMatch(prefix.mPrefix) >= prefix.mLength))
                {
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                bool added = false;

                for (slaacAddr = &mAddresses[0]; slaacAddr < &mAddresses[OT_ARRAY_LENGTH(mAddresses)]; slaacAddr++)
                {
                    if (slaacAddr->mValid)
                    {
                        continue;
                    }

                    memset(slaacAddr, 0, sizeof(*slaacAddr));
                    memcpy(&slaacAddr->mAddress, &prefix.mPrefix, BitVectorBytes(prefix.mLength));

                    slaacAddr->mPrefixLength = prefix.mLength;
                    slaacAddr->mPreferred    = config.mPreferred;
                    slaacAddr->mValid        = true;

                    GenerateIid(*slaacAddr);

                    otLogInfoUtil("SLAAC: Adding address %s", slaacAddr->GetAddress().ToString().AsCString());

                    netif.AddUnicastAddress(*slaacAddr);

                    added = true;
                    break;
                }

                if (!added)
                {
                    otLogWarnUtil("SLAAC: Failed to add - max %d addresses supported and already in use",
                                  OT_ARRAY_LENGTH(mAddresses));
                }
            }
        }
    }
}

void Slaac::GenerateIid(Ip6::NetifUnicastAddress &aAddress) const
{
    /*
     *  This method generates a semantically opaque IID per RFC 7217.
     *
     * RID = F(Prefix, Net_Iface, Network_ID, DAD_Counter, secret_key)
     *
     *  - RID is random (but stable) Identifier.
     *  - For pseudo-random function `F()` SHA-256 is used in this method.
     *  - `Net_Iface` is set to constant string "wpan".
     *  - `Network_ID` is not used (optional per RF-7217).
     *  - The `secret_key` is randomly generated on first use (using true
     *    random number generator) and saved in non-volatile settings for
     *    future use.
     *
     */

    const uint8_t  netIface[] = {'w', 'p', 'a', 'n'};
    uint16_t       dadCounter;
    IidSecretKey   secretKey;
    Crypto::Sha256 sha256;
    uint8_t        hash[Crypto::Sha256::kHashSize];

    OT_STATIC_ASSERT(sizeof(hash) >= Ip6::Address::kInterfaceIdentifierSize,
                     "SHA-256 hash size is too small to use as IPv6 address IID");

    GetIidSecretKey(secretKey);

    for (dadCounter = 0; dadCounter < kMaxIidCreationAttempts; dadCounter++)
    {
        sha256.Start();
        sha256.Update(aAddress.mAddress.mFields.m8, BitVectorBytes(aAddress.mPrefixLength));
        sha256.Update(netIface, sizeof(netIface));
        sha256.Update(reinterpret_cast<uint8_t *>(&dadCounter), sizeof(dadCounter));
        sha256.Update(secretKey.m8, sizeof(IidSecretKey));
        sha256.Finish(hash);

        aAddress.GetAddress().SetIid(&hash[0]);

        // Exit and return the address if the IID is not reserved,
        // otherwise, try again with a new dadCounter

        VerifyOrExit(aAddress.GetAddress().IsIidReserved());
    }

    otLogWarnUtil("SLAAC: Failed to generate a non-reserved IID after %d attempts", dadCounter);
    Random::FillBuffer(hash, Ip6::Address::kInterfaceIdentifierSize);
    aAddress.GetAddress().SetIid(&hash[0]);

exit:
    return;
}

void Slaac::GetIidSecretKey(IidSecretKey &aKey) const
{
    otError   error;
    Settings &settings = GetInstance().GetSettings();

    error = settings.ReadSlaacIidSecretKey(aKey);
    VerifyOrExit(error != OT_ERROR_NONE);

    // If there is no previously saved secret key, generate
    // a random one and save it.

    error = otPlatRandomGetTrue(aKey.m8, sizeof(IidSecretKey));

    if (error != OT_ERROR_NONE)
    {
        Random::FillBuffer(aKey.m8, sizeof(IidSecretKey));
    }

    settings.SaveSlaacIidSecretKey(aKey);

    otLogInfoUtil("SLAAC: Generated and saved secret key");

exit:
    return;
}

} // namespace Utils
} // namespace ot

#endif // OPENTHREAD_CONFIG_ENABLE_SLAAC
