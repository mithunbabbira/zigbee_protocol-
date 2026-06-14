import * as m from "zigbee-herdsman-converters/lib/modernExtend";

/** Matches SHELF_HA_LIGHT_EP_ID in components/shelf_zigbee/include/shelf_zigbee.h */
const SHELF_LIGHT_ENDPOINT = 10;

/**
 * Shelf ESP32-C6 on/off node.
 *
 * Stability notes (Zigbee2MQTT docs):
 * - disableDefaultResponse: ESP nodes may apply On/Off without a timely Default Response.
 * - bind + reporting on genOnOff: coordinator receives state reports after local changes.
 * - endpoint 10: must match firmware HA light endpoint.
 */
/** @type {import('zigbee-herdsman-converters/lib/types').DefinitionWithExtend} */
export default {
    zigbeeModel: ["esp32c6", "SHELF-NODE"],
    fingerprint: [
        {modelID: "esp32c6", manufacturerName: "SHELF_MGMT"},
        {modelID: "SHELF-NODE", manufacturerName: "SHELF_MGMT"},
    ],
    model: "SHELF-NODE",
    vendor: "SHELF_MGMT",
    description: "Shelf management ESP32-C6 on/off node",
    meta: {
        disableDefaultResponse: true,
        timeout: 30000,
    },
    extend: [
        m.deviceEndpoints({
            endpoints: {
                light: SHELF_LIGHT_ENDPOINT,
            },
        }),
        m.onOff({
            powerOnBehavior: false,
            configureReporting: true,
            skipDuplicateTransaction: true,
            endpointNames: ["light"],
        }),
        m.bindCluster({
            cluster: "genOnOff",
            clusterType: "input",
            endpointNames: ["light"],
        }),
    ],
};
