/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.telephony;

import android.annotation.SdkConstant;
import android.annotation.SdkConstant.SdkConstantType;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemProperties;
import android.provider.SpnProvider;
import android.text.TextUtils;
import android.util.Log;

import com.android.internal.telephony.IPhoneSubInfoMSim;
import com.android.internal.telephony.ITelephonyMSim;
import com.android.internal.telephony.ITelephonyRegistryMSim;
import com.android.internal.telephony.MSimConstants;
import com.android.internal.telephony.Phone;
import com.android.internal.telephony.PhoneFactory;
import com.android.internal.telephony.TelephonyProperties;

import java.util.List;

import com.qrd.plugin.feature_query.FeatureQuery;

/**
 * Provides access to information about the telephony services on
 * the device. Applications can use the methods in this class to
 * determine telephony services and states, as well as to access some
 * types of subscriber information. Applications can also register
 * a listener to receive notification of telephony state changes.
 * <p>
 * You do not instantiate this class directly; instead, you retrieve
 * a reference to an instance through
 * {@link android.content.Context#getSystemService
 * Context.getSystemService(Context.MSIM_TELEPHONY_SERVICE)}.
 * <p>
 * Note that access to some telephony information is
 * permission-protected. Your application cannot access the protected
 * information unless it has the appropriate permissions declared in
 * its manifest file. Where permissions apply, they are noted in the
 * the methods through which you access the protected information.
 */
public class MSimTelephonyManager extends TelephonyManager {
    /** @hide */
    protected static ITelephonyRegistryMSim sRegistryMsim;

    /** @hide */
    public MSimTelephonyManager(Context context) {
        super(context);
            sRegistryMsim = ITelephonyRegistryMSim.Stub.asInterface(ServiceManager.getService(
                    "telephony.msim.registry"));
    }

    /** @hide */
    private MSimTelephonyManager() {
    }

    private static MSimTelephonyManager sInstance = new MSimTelephonyManager();

    /** @hide
    /* @deprecated - use getSystemService as described above */
    public static MSimTelephonyManager getDefault() {
        return sInstance;
    }

    /**
     * Returns the number of phones available.
     * Returns 1 for Single standby mode (Single SIM functionality)
     * Returns 2 for Dual standby mode.(Dual SIM functionality)
     *
     * @hide
     */
    public int getPhoneCount() {
         return MSimConstants.MAX_PHONE_COUNT_DS;
    }

    /**
     * Returns the unique device ID of a subscription, for example, the IMEI for
     * GSM and the MEID for CDMA phones. Return null if device ID is not available.
     *
     * <p>Requires Permission:
     *   {@link android.Manifest.permission#READ_PHONE_STATE READ_PHONE_STATE}
     *
     * @param subscription of which deviceID is returned
     */
    public String getDeviceId(int subscription) {
        if (!isMultiSimEnabled) return super.getDeviceId();

        try {
            return getMSimSubscriberInfo().getDeviceId(subscription);
        } catch (RemoteException ex) {
            return null;
        } catch (NullPointerException ex) {
            return null;
        }
    }

    /**
     * Returns the location of the device of a subscription. Return
     * null if current location is not available.
     *
     * <p>
     * Requires Permission:
     * {@link android.Manifest.permission#ACCESS_COARSE_LOCATION
     * ACCESS_COARSE_LOCATION} or
     * {@link android.Manifest.permission#ACCESS_COARSE_LOCATION
     * ACCESS_FINE_LOCATION}.
     *
     * @param subscription
     *            of which the device current location is returned
     * @hide
     */
    public CellLocation getCellLocation(int subscription) {
        ITelephonyMSim iTelephony = null;
        try {
            if (FeatureQuery.FEATURE_SECURITY 
                    && checkLocationSecurityPermission() != PackageManager.PERMISSION_GRANTED)
                return null;

            iTelephony = ITelephonyMSim.Stub.asInterface(ServiceManager
                    .getService(Context.MSIM_TELEPHONY_SERVICE));
            Bundle bundle = iTelephony.getCellLocation(subscription);
            CellLocation cl = CellLocation.newFromBundle(bundle);
            if (cl.isEmpty())
                return null;
            return cl;
        } catch (RemoteException ex) {
            return null;
        } catch (NullPointerException ex) {
            return null;
        }
    }

    private int getPreferredSubscription() {
        for (int i = 0; i < 2; i++) {
            if (SIM_STATE_ABSENT != getSimState(i)) {
                return i;
            }
        }
        return 0;
    }

    /**
     * @hide
     */
    @Override
    public String getDeviceId() {
        if (!isMultiSimEnabled()) {
            return super.getDeviceId();
        } else {
            return getDeviceId(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public CellLocation getCellLocation() {
        if (!isMultiSimEnabled()) {
            return super.getCellLocation();
        } else {
            return getCellLocation(getPreferredSubscription());
        }

    }

    /**
     * @hide
     */
    @Override
    public int getCurrentPhoneType() {
        if (!isMultiSimEnabled()) {
            return super.getCurrentPhoneType();
        } else {
            return getCurrentPhoneType(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public String getNetworkOperatorName() {
        if (!isMultiSimEnabled()) {
            return super.getNetworkOperatorName();
        } else {
            return getNetworkOperatorName(getPreferredSubscription());
        }

    }

    /**
     * @hide
     */
    @Override
    public String getNetworkOperator() {
        if (!isMultiSimEnabled()) {
            return super.getNetworkOperator();
        } else {
            return getNetworkOperator(getPreferredSubscription());
        }

    }

    /**
     * @hide
     */
    @Override
    public boolean isNetworkRoaming() {
        if (!isMultiSimEnabled()) {
            return super.isNetworkRoaming();
        } else {
            return isNetworkRoaming(getPreferredSubscription());
        }

    }

    /**
     * @hide
     */
    @Override
    public int getNetworkType() {
        if (!isMultiSimEnabled()) {
            return super.getNetworkType();
        } else {
            return getNetworkType(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public String getNetworkTypeName() {
        if (!isMultiSimEnabled()) {
            return super.getNetworkTypeName();
        } else {
            return getNetworkTypeName(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public boolean hasIccCard() {
        if (!isMultiSimEnabled()) {
            return super.hasIccCard();
        } else {
            return hasIccCard(getPreferredSubscription());
        }

    }

    /**
     * @hide
     */
    @Override
    public int getSimState() {
        if (!isMultiSimEnabled()) {
            return super.getSimState();
        } else {
            return getSimState(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public String getSimOperator() {
        if (!isMultiSimEnabled()) {
            return super.getSimOperator();
        } else {
            return getSimOperator(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public String getSimOperatorName() {
        if (!isMultiSimEnabled()) {
            return super.getSimOperatorName();
        } else {
            return getSimOperatorName(getPreferredSubscription());
        }

    }

    /**
     * @hide
     */
    @Override
    public String getSimSerialNumber() {
        if (!isMultiSimEnabled()) {
            return super.getSimSerialNumber();
        } else {
            return getSimSerialNumber(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public int getLteOnCdmaMode() {
        if (!isMultiSimEnabled()) {
            return super.getLteOnCdmaMode();
        } else {
            return getLteOnCdmaMode(getPreferredSubscription());
        }

    }

    /**
     * @hide
     */
    @Override
    public String getSubscriberId() {
        if (!isMultiSimEnabled()) {
            return super.getSubscriberId();
        } else {
            return getSubscriberId(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public String getCardType() {
        if (!isMultiSimEnabled()) {
            return super.getCardType();
        } else {
            return getCardType(getPreferredSubscription());
        }

    }

    /**
     * @hide
     */
    @Override
    public String getLine1Number() {
        if (!isMultiSimEnabled()) {
            return super.getLine1Number();
        } else {
            return getLine1Number(getPreferredSubscription());
        }

    }

    /**
     * @hide
     */
    @Override
    public String getLine1AlphaTag() {
        if (!isMultiSimEnabled()) {
            return super.getLine1AlphaTag();
        } else {
            return getLine1AlphaTag(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public String getVoiceMailNumber() {
        if (!isMultiSimEnabled()) {
            return super.getVoiceMailNumber();
        } else {
            return getVoiceMailNumber(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public String getCompleteVoiceMailNumber() {
        if (!isMultiSimEnabled()) {
            return super.getCompleteVoiceMailNumber();
        } else {
            return getCompleteVoiceMailNumber(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public int getVoiceMessageCount() {
        if (!isMultiSimEnabled()) {
            return super.getVoiceMessageCount();
        } else {
            return getVoiceMessageCount(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public String getVoiceMailAlphaTag() {
        if (!isMultiSimEnabled()) {
            return super.getVoiceMailAlphaTag();
        } else {
            return getVoiceMailAlphaTag(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public int getCallState() {
        if (!isMultiSimEnabled()) {
            return super.getCallState();
        } else {
            return getCallState(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public int getCdmaEriIconIndex() {
        if (!isMultiSimEnabled()) {
            return super.getCdmaEriIconIndex();
        } else {
            return getCdmaEriIconIndex(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public int getCdmaEriIconMode() {
        if (!isMultiSimEnabled()) {
            return super.getCdmaEriIconMode();
        } else {
            return getCdmaEriIconMode(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public String getCdmaEriText() {
        if (!isMultiSimEnabled()) {
            return super.getCdmaEriText();
        } else {
            return getCdmaEriText(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    @Override
    public String getSimCountryIso() {
        if (!isMultiSimEnabled()) {
            return super.getSimCountryIso();
        } else {
            return getSimCountryIso(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    public String getSimCountryIso(int subscription) {
        if (!isMultiSimEnabled()) {
            return super.getSimCountryIso();
        }
        return getTelephonyProperty(
                subscription == 0 ? TelephonyProperties.PROPERTY_ICC_OPERATOR_ISO_COUNTRY
                        : TelephonyProperties.PROPERTY_ICC2_OPERATOR_ISO_COUNTRY, "");
    }

    /**
     * @hide
     */
    @Override
    public String getNetworkCountryIso() {
        if (!isMultiSimEnabled()) {
            return super.getNetworkCountryIso();
        } else {
            return getNetworkCountryIso(getPreferredSubscription());
        }
    }

    /**
     * @hide
     */
    public String getNetworkCountryIso(int subscription) {
        if (!isMultiSimEnabled()) {
            return super.getNetworkCountryIso();
        }
        return getTelephonyProperty(
                subscription == 0 ? TelephonyProperties.PROPERTY_OPERATOR_ISO_COUNTRY
                        : TelephonyProperties.PROPERTY_OPERATOR2_ISO_COUNTRY, "");
    }

    /**
     * Returns a constant indicating the device phone type for a subscription.
     *
     * @see #PHONE_TYPE_NONE
     * @see #PHONE_TYPE_GSM
     * @see #PHONE_TYPE_CDMA
     *
     * @param subscription for which phone type is returned
     * @hide
     */
    public int getCurrentPhoneType(int subscription) {
        if (!isMultiSimEnabled) return super.getCurrentPhoneType();

        try{
            ITelephonyMSim telephony = getITelephonyMSim();
            if (telephony != null) {
                return telephony.getActivePhoneType(subscription);
            } else {
                // This can happen when the ITelephonyMSim interface is not up yet.
                return getPhoneTypeFromProperty(subscription);
            }
        } catch (RemoteException ex) {
            // This shouldn't happen in the normal case, as a backup we
            // read from the system property.
            return getPhoneTypeFromProperty(subscription);
        } catch (NullPointerException ex) {
            // This shouldn't happen in the normal case, as a backup we
            // read from the system property.
            return getPhoneTypeFromProperty(subscription);
        }
    }

    private int getPhoneTypeFromProperty(int subscription) {
        String type = getTelephonyProperty(
                subscription == 0 ? TelephonyProperties.CURRENT_ACTIVE_PHONE
                        : TelephonyProperties.CURRENT2_ACTIVE_PHONE, null);
        if (!TextUtils.isEmpty(type)) {
            return (Integer.parseInt(type));
        } else {
            return getPhoneTypeFromNetworkType(subscription);
        }
    }

    private int getPhoneTypeFromNetworkType(int subscription) {
        // When the system property CURRENT_ACTIVE_PHONE, has not been set,
        // use the system property for default network type.
        // This is a fail safe, and can only happen at first boot.
        String mode = getTelephonyProperty("ro.telephony.default_network", null);
        if (!TextUtils.isEmpty(mode)) {
            return PhoneFactory.getPhoneType(Integer.parseInt(mode));
        }
        return PHONE_TYPE_NONE;
    }

    //
    //
    // Current Network
    //
    //

    /**
     * Returns the alphabetic name of current registered operator
     * for a particular subscription.
     * <p>
     * Availability: Only when user is registered to a network. Result may be
     * unreliable on CDMA networks (use {@link #getPhoneType()} to determine if
     * on a CDMA network).
     * @param subscription
     */
    public String getNetworkOperatorName(int subscription) {
        if (!isMultiSimEnabled) return super.getNetworkOperatorName();

        return getTelephonyProperty(subscription == 0 ? TelephonyProperties.PROPERTY_OPERATOR_ALPHA
                : TelephonyProperties.PROPERTY_OPERATOR2_ALPHA, "");
    }

    /**
     * Returns the numeric name (MCC+MNC) of current registered operator
     * for a particular subscription.
     * <p>
     * Availability: Only when user is registered to a network. Result may be
     * unreliable on CDMA networks (use {@link #getPhoneType()} to determine if
     * on a CDMA network).
     *
     * @param subscription
     */
    public String getNetworkOperator(int subscription) {
        if (!isMultiSimEnabled) return super.getNetworkOperator();

        return getTelephonyProperty(
                subscription == 0 ? TelephonyProperties.PROPERTY_OPERATOR_NUMERIC
                        : TelephonyProperties.PROPERTY_OPERATOR2_NUMERIC, "");
     }

    /**
     * Returns true if the device is considered roaming on the current
     * network for a subscription.
     * <p>
     * Availability: Only when user registered to a network.
     *
     * @param subscription
     */
    public boolean isNetworkRoaming(int subscription) {
        if (!isMultiSimEnabled) return super.isNetworkRoaming();
        return "true".equals(getTelephonyProperty(
                subscription == 0 ? TelephonyProperties.PROPERTY_OPERATOR_ISROAMING
                        : TelephonyProperties.PROPERTY_OPERATOR2_ISROAMING, null));
    }

    /**
     * Returns a constant indicating the radio technology (network type)
     * currently in use on the device for data transmission for a subscription
     * @return the network type
     *
     * @param subscription for which network type is returned
     *
     * @see #NETWORK_TYPE_UNKNOWN
     * @see #NETWORK_TYPE_GPRS
     * @see #NETWORK_TYPE_EDGE
     * @see #NETWORK_TYPE_UMTS
     * @see #NETWORK_TYPE_HSDPA
     * @see #NETWORK_TYPE_HSUPA
     * @see #NETWORK_TYPE_HSPA
     * @see #NETWORK_TYPE_CDMA
     * @see #NETWORK_TYPE_EVDO_0
     * @see #NETWORK_TYPE_EVDO_A
     * @see #NETWORK_TYPE_EVDO_B
     * @see #NETWORK_TYPE_1xRTT
     * @see #NETWORK_TYPE_EHRPD
     * @see #NETWORK_TYPE_LTE
     */
    public static int getNetworkType(int subscription) {
        if (!isMultiSimEnabled) return TelephonyManager.getDefault().getNetworkType();

        ITelephonyMSim iTelephony = null;
        try {
            iTelephony = ITelephonyMSim.Stub.asInterface(
                    ServiceManager.getService(Context.MSIM_TELEPHONY_SERVICE));
            return iTelephony.getNetworkType(subscription);
        } catch (RemoteException ex) {
            // This shouldn't happen in the normal case
            return NETWORK_TYPE_UNKNOWN;
        } catch (NullPointerException ex) {
            // This shouldn't happen in the normal case
            return NETWORK_TYPE_UNKNOWN;
        }
    }

    /**
     * Returns a string representation of the radio technology (network type)
     * currently in use on the device.
     * @param subscription for which network type is returned
     * @return the name of the radio technology
     *
     * @hide pending API council review
     */
    public static String getNetworkTypeName(int subscription) {
        if (!isMultiSimEnabled) return TelephonyManager.getDefault().getNetworkTypeName();

        return TelephonyManager.getNetworkTypeName(getNetworkType(subscription));
    }

    //
    //
    // SIM Card
    //
    //

    /**
     * @return true if a ICC card is present for a subscription
     *
     * @param subscription for which icc card presence is checked
     */
    public boolean hasIccCard(int subscription) {
        if (!isMultiSimEnabled) return super.hasIccCard();

        try {
            return getITelephonyMSim().hasIccCard(subscription);
        } catch (RemoteException ex) {
            // Assume no ICC card if remote exception which shouldn't happen
            return false;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return false;
        }
    }

    /**
     * Returns a constant indicating the state of the
     * device SIM card in a slot.
     *
     * @param slotId
     *
     * @see #SIM_STATE_UNKNOWN
     * @see #SIM_STATE_ABSENT
     * @see #SIM_STATE_CARD_IO_ERROR
     */
    public int getSimState(int slotId) {
        if (!isMultiSimEnabled) return super.getSimState();

        String prop = getTelephonyProperty(slotId == 0 ? TelephonyProperties.PROPERTY_SIM_STATE
                : TelephonyProperties.PROPERTY_SIM2_STATE, "");
        if ("ABSENT".equals(prop)) {
            return SIM_STATE_ABSENT;
        }
        else if ("PIN_REQUIRED".equals(prop)) {
            return SIM_STATE_PIN_REQUIRED;
        }
        else if ("PUK_REQUIRED".equals(prop)) {
            return SIM_STATE_PUK_REQUIRED;
        }
        else if ("NETWORK_LOCKED".equals(prop)) {
            return SIM_STATE_NETWORK_LOCKED;
        }
        else if ("READY".equals(prop)) {
            return SIM_STATE_READY;
        }
        else if ("CARD_IO_ERROR".equals(prop)) {
            return SIM_STATE_CARD_IO_ERROR;
        }
        else if ("CARD_DEACTIVATED".equals(prop)) {
            return SIM_STATE_DEACTIVATED;
        }
        else {
            return SIM_STATE_UNKNOWN;
        }
    }

    /**
     * Return if the current radio is LTE on CDMA. This
     * is a tri-state return value as for a period of time
     * the mode may be unknown.
     *
     * @return {@link Phone#LTE_ON_CDMA_UNKNOWN}, {@link Phone#LTE_ON_CDMA_FALSE}
     * or {@link Phone#LTE_ON_CDMA_TRUE}
     *
     * @hide
     */
    public int getLteOnCdmaMode(int subscription) {
        if (!isMultiSimEnabled) return super.getLteOnCdmaMode();
        try {
            return getITelephonyMSim().getLteOnCdmaMode(subscription);
        } catch (RemoteException ex) {
            // Assume no ICC card if remote exception which shouldn't happen
            return Phone.LTE_ON_CDMA_UNKNOWN;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return Phone.LTE_ON_CDMA_UNKNOWN;
        }
    }

    /**
     * Returns the serial number for the given subscription, if applicable. Return null if it is
     * unavailable.
     * <p>
     * Requires Permission:
     *   {@link android.Manifest.permission#READ_PHONE_STATE READ_PHONE_STATE}
     */
    public String getSimSerialNumber(int subscription) {
        if (!isMultiSimEnabled) return super.getSimSerialNumber();
        try {
            return getMSimSubscriberInfo().getIccSerialNumber(subscription);
        } catch (RemoteException ex) {
            return null;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return null;
        }
    }

    //
    //
    // Subscriber Info
    //
    //

    /**
     * Returns the unique subscriber ID, for example, the IMSI for a GSM phone
     * for a subscription.
     * Return null if it is unavailable.
     * <p>
     * Requires Permission:
     *   {@link android.Manifest.permission#READ_PHONE_STATE READ_PHONE_STATE}
     *
     * @param subscription whose subscriber id is returned
     */
    public String getSubscriberId(int subscription) {
        if (!isMultiSimEnabled) return super.getSubscriberId();
        try {
            return getMSimSubscriberInfo().getSubscriberId(subscription);
        } catch (RemoteException ex) {
            return null;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return null;
        }
    }

        /**
     * Returns the sim card type for a subscription, for example, USIM or RUIM.
     * Return null if it is unavailable.
     * <p>
     * Requires Permission:
     *   {@link android.Manifest.permission#READ_PHONE_STATE READ_PHONE_STATE}
     */
    /** @hide */
    public String getCardType(int subscription) {
        if (!isMultiSimEnabled) return getCardType();
        try {
            return getMSimSubscriberInfo().getCardType(subscription);
        } catch (RemoteException ex) {
            return null;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return null;
        }
    }

    /**
     * Returns the phone number string for line 1, for example, the MSISDN
     * for a GSM phone for a particular subscription. Return null if it is unavailable.
     * <p>
     * Requires Permission:
     *   {@link android.Manifest.permission#READ_PHONE_STATE READ_PHONE_STATE}
     *
     * @param subscription whose phone number for line 1 is returned
     */
    public String getLine1Number(int subscription) {
        if (!isMultiSimEnabled) return super.getLine1Number();
        try {
            return getMSimSubscriberInfo().getLine1Number(subscription);
        } catch (RemoteException ex) {
            return null;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return null;
        }
    }

    /**
     * Returns the alphabetic identifier associated with the line 1 number
     * for a subscription.
     * Return null if it is unavailable.
     * <p>
     * Requires Permission:
     *   {@link android.Manifest.permission#READ_PHONE_STATE READ_PHONE_STATE}
     * @param subscription whose alphabetic identifier associated with line 1 is returned
     * @hide
     * nobody seems to call this.
     */
    public String getLine1AlphaTag(int subscription) {
        if (!isMultiSimEnabled) return super.getLine1AlphaTag();
        try {
            return getMSimSubscriberInfo().getLine1AlphaTag(subscription);
        } catch (RemoteException ex) {
            return null;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return null;
        }
    }

    /**
     * Returns the voice mail number for a subscription.
     * Return null if it is unavailable.
     * <p>
     * Requires Permission:
     *   {@link android.Manifest.permission#READ_PHONE_STATE READ_PHONE_STATE}
     * @param subscription whose voice mail number is returned
     */
    public String getVoiceMailNumber(int subscription) {
        if (!isMultiSimEnabled) return super.getVoiceMailNumber();
        try {
            return getMSimSubscriberInfo().getVoiceMailNumber(subscription);
        } catch (RemoteException ex) {
            return null;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return null;
        }
    }

    /**
     * Returns the complete voice mail number. Return null if it is unavailable.
     * <p>
     * Requires Permission:
     *   {@link android.Manifest.permission#CALL_PRIVILEGED CALL_PRIVILEGED}
     *
     * @param subscription
     * @hide
     */
    public String getCompleteVoiceMailNumber(int subscription) {
        if (!isMultiSimEnabled) return super.getCompleteVoiceMailNumber();
        try {
            return getMSimSubscriberInfo().getCompleteVoiceMailNumber(subscription);
        } catch (RemoteException ex) {
            return null;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return null;
        }
    }


    /**
     * Returns the voice mail count for a subscription. Return 0 if unavailable.
     * <p>
     * Requires Permission:
     *   {@link android.Manifest.permission#READ_PHONE_STATE READ_PHONE_STATE}
     * @param subscription whose voice message count is returned
     * @hide
     */
    public int getVoiceMessageCount(int subscription) {
        if (!isMultiSimEnabled) return super.getVoiceMessageCount();
        try {
            return getITelephonyMSim().getVoiceMessageCount(subscription);
        } catch (RemoteException ex) {
            return 0;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return 0;
        }
    }

    /**
     * Retrieves the alphabetic identifier associated with the voice
     * mail number for a subscription.
     * <p>
     * Requires Permission:
     *   {@link android.Manifest.permission#READ_PHONE_STATE READ_PHONE_STATE}
     * @param subscription whose alphabetic identifier associated with the voice mail number is returned
     */
    public String getVoiceMailAlphaTag(int subscription) {
        if (!isMultiSimEnabled) return super.getVoiceMailAlphaTag();
        try {
            return getMSimSubscriberInfo().getVoiceMailAlphaTag(subscription);
        } catch (RemoteException ex) {
            return null;
        } catch (NullPointerException ex) {
            // This could happen before phone restarts due to crashing
            return null;
        }
    }

    /**
     * @hide
     */
    protected IPhoneSubInfoMSim getMSimSubscriberInfo() {
        // get it each time because that process crashes a lot
        return IPhoneSubInfoMSim.Stub.asInterface(ServiceManager.getService("iphonesubinfo_msim"));
    }

    /**
     * Returns a constant indicating the call state (cellular) on the device
     * for a subscription.
     *
     * @param subscription whose call state is returned
     */
    public int getCallState(int subscription) {
        if (!isMultiSimEnabled) return super.getCallState();
        try {
            return getITelephonyMSim().getCallState(subscription);
        } catch (RemoteException ex) {
            // the phone process is restarting.
            return CALL_STATE_IDLE;
        } catch (NullPointerException ex) {
          // the phone process is restarting.
          return CALL_STATE_IDLE;
      }
    }

    /**
     * Returns a constant indicating the type of activity on a data connection
     * (cellular).
     *
     * @see #DATA_ACTIVITY_NONE
     * @see #DATA_ACTIVITY_IN
     * @see #DATA_ACTIVITY_OUT
     * @see #DATA_ACTIVITY_INOUT
     * @see #DATA_ACTIVITY_DORMANT
     */
    public int getDataActivity() {
        if (!isMultiSimEnabled) return super.getDataActivity();
        try {
            return getITelephonyMSim().getDataActivity();
        } catch (RemoteException ex) {
            // the phone process is restarting.
            return DATA_ACTIVITY_NONE;
        } catch (NullPointerException ex) {
          // the phone process is restarting.
          return DATA_ACTIVITY_NONE;
      }
    }

    /**
     * Returns a constant indicating the current data connection state
     * (cellular).
     *
     * @see #DATA_DISCONNECTED
     * @see #DATA_CONNECTING
     * @see #DATA_CONNECTED
     * @see #DATA_SUSPENDED
     */
    public int getDataState() {
        if (!isMultiSimEnabled) return super.getDataState();
        try {
            return getITelephonyMSim().getDataState();
        } catch (RemoteException ex) {
            // the phone process is restarting.
            return DATA_DISCONNECTED;
        } catch (NullPointerException ex) {
            return DATA_DISCONNECTED;
        }
    }

    private ITelephonyMSim getITelephonyMSim() {
        return ITelephonyMSim.Stub.asInterface(ServiceManager.getService(Context.MSIM_TELEPHONY_SERVICE));
    }

    //
    //
    // PhoneStateListener
    //
    //

    /**
     * Registers a listener object to receive notification of changes
     * in specified telephony states.
     * <p>
     * To register a listener, pass a {@link PhoneStateListener}
     * and specify at least one telephony state of interest in
     * the events argument.
     *
     * At registration, and when a specified telephony state
     * changes, the telephony manager invokes the appropriate
     * callback method on the listener object and passes the
     * current (udpated) values.
     * <p>
     * To unregister a listener, pass the listener object and set the
     * events argument to
     * {@link PhoneStateListener#LISTEN_NONE LISTEN_NONE} (0).
     *
     * @param listener The {@link PhoneStateListener} object to register
     *                 (or unregister)
     * @param events The telephony state(s) of interest to the listener,
     *               as a bitwise-OR combination of {@link PhoneStateListener}
     *               LISTEN_ flags.
     */
    @Override
    public void listen(PhoneStateListener listener, int events) {
        if (!isMultiSimEnabled) {
            super.listen(listener, events);
            return;
        }
        String pkgForDebug = sContext != null ? sContext.getPackageName() : "<unknown>";
        try {
            Boolean notifyNow = (getITelephonyMSim() != null);
            sRegistryMsim.listen(pkgForDebug, listener.callback, events, notifyNow,
                                           listener.mSubscription);
        } catch (RemoteException ex) {
            // system process dead
        } catch (NullPointerException ex) {
            // system process dead
        }
    }

    /**
     * Returns the CDMA ERI icon index to display for a subscription
     *
     * @hide
     */
    public int getCdmaEriIconIndex(int subscription) {
        if (!isMultiSimEnabled) return super.getCdmaEriIconIndex();
        try {
            return getITelephonyMSim().getCdmaEriIconIndex(subscription);
        } catch (RemoteException ex) {
            // the phone process is restarting.
            return -1;
        } catch (NullPointerException ex) {
            return -1;
        }
    }

    /**
     * Returns the CDMA ERI icon mode for a subscription.
     * 0 - ON
     * 1 - FLASHING
     *
     * @hide
     */
    public int getCdmaEriIconMode(int subscription) {
        if (!isMultiSimEnabled) return super.getCdmaEriIconMode();
        try {
            return getITelephonyMSim().getCdmaEriIconMode(subscription);
        } catch (RemoteException ex) {
            // the phone process is restarting.
            return -1;
        } catch (NullPointerException ex) {
            return -1;
        }
    }

    /**
     * Returns the CDMA ERI text, of a subscription
     *
     * @hide
     */
    public String getCdmaEriText(int subscription) {
        if (!isMultiSimEnabled) return super.getCdmaEriText();
        try {
            return getITelephonyMSim().getCdmaEriText(subscription);
        } catch (RemoteException ex) {
            // the phone process is restarting.
            return null;
        } catch (NullPointerException ex) {
            return null;
        }
    }

    /**
     * Returns Default subscription.
     * Returns default value 0, if default subscription is not available
     */
    public int getDefaultSubscription() {
        try {
            return getITelephonyMSim().getDefaultSubscription();
        } catch (RemoteException ex) {
            return MSimConstants.DEFAULT_SUBSCRIPTION;
        } catch (NullPointerException ex) {
            return MSimConstants.DEFAULT_SUBSCRIPTION;
        }
    }

    /**
     * Returns the designated data subscription.
     */
    public int getPreferredDataSubscription() {
        try {
            return getITelephonyMSim().getPreferredDataSubscription();
        } catch (RemoteException ex) {
            return MSimConstants.DEFAULT_SUBSCRIPTION;
        } catch (NullPointerException ex) {
            return MSimConstants.DEFAULT_SUBSCRIPTION;
        }
    }

    /**
     * Sets the designated data subscription.
     */
    public boolean setPreferredDataSubscription(int subscription) {
        try {
            return getITelephonyMSim().setPreferredDataSubscription(subscription);
        } catch (RemoteException ex) {
            return false;
        } catch (NullPointerException ex) {
            return false;
        }
    }

    /**
     * Returns the preferred voice subscription.
     */
    public int getPreferredVoiceSubscription() {
        try {
            return getITelephonyMSim().getPreferredVoiceSubscription();
        } catch (RemoteException ex) {
            return MSimConstants.DEFAULT_SUBSCRIPTION;
        } catch (NullPointerException ex) {
            return MSimConstants.DEFAULT_SUBSCRIPTION;
        }
    }

    /**
     * Returns the MCC+MNC (mobile country code + mobile network code) of the
     * provider of the SIM for a particular subscription. 5 or 6 decimal digits.
     * <p>
     * Availability: SIM state must be {@link #SIM_STATE_READY}
     *
     * @see #getSimState
     *
     * @param subscription for which provider's MCC+MNC is returned
     * @hide
     */
    public String getSimOperator(int subscription) {
        if (!isMultiSimEnabled) return getSimOperator();
        return getTelephonyProperty(
                subscription == 0 ? TelephonyProperties.PROPERTY_ICC_OPERATOR_NUMERIC
                        : TelephonyProperties.PROPERTY_ICC2_OPERATOR_NUMERIC, "");
    }

    /**
     * Returns the Service Provider Name (SPN) of a subscription.
     * <p>
     * Availability: SIM state must be {@link #SIM_STATE_READY}
     *
     * @see #getSimState
     *
     * @hide
     */
    public String getSimOperatorName(int subscription) {
        if (!isMultiSimEnabled) return getSimOperatorName();
        String alpha = getTelephonyProperty(
                subscription == 0 ? TelephonyProperties.PROPERTY_ICC_OPERATOR_ALPHA
                        : TelephonyProperties.PROPERTY_ICC2_OPERATOR_ALPHA, "");
        if ("".equals(alpha)) {
            String numeric = getSimOperator(subscription);
            if (numeric != null && numeric.length() > 3)
                alpha = (String) SpnProvider.getSPNByMCCMNC(sContext, numeric);
        }
        return alpha;
    }
}

