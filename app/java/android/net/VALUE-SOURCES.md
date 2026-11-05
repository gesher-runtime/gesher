# Connectivity value APIs

MacAddress, StaticIpConfiguration and IpConfiguration restore the Connectivity
module's actual Android 16 r4 value implementations. MacAddressUtils and
InetAddressUtils are their real upstream helpers; no network or Wi-Fi service
is required to construct, validate, compare or parcel these values. Copyright
and Apache-2.0 headers are retained. Exact source URLs and upstream/local SHA-256
receipts are in `docs/research/aosp-network-values-2026-11-05.json`.

IpConfiguration has two deliberate corrections to the pinned implementation:
hashCode accepts the normal absent proxy, and Builder recognizes Uri.EMPTY as
the direct proxy sentinel used by ProxyInfo (otherwise every direct proxy is
classified as PAC). RouteInfo gains the LinkAddress constructor and host-route
helper needed to express a gateway outside the connected subnet.

NetworkValueContractTest passed 30 assertions on the isolated Windows runtime:
MAC formatting/types/copying/masks/EUI64/rejected inputs; full parcel consumption;
static connected, host and default routes; DNS copy ownership; IPv6 rejection;
DHCP, direct proxy and PAC configuration. Output is retained under
`out/stability/java-values-candidate`. This verifies value behavior, not applying
these configurations to a Windows interface.
