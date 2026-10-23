// Gesher: what system/core/toolbox/generate-input.h-labels.py generates from
// the kernel's input headers, for the event types only (see
// linux/input-event-codes.h here): the evdev code tables are empty, so evdev
// codes print as numbers. Android's key code names are libinput's own.
static struct label input_prop_labels[] = {
    LABEL_END,
};
static struct label ev_labels[] = {
    LABEL(EV_SYN), LABEL(EV_KEY), LABEL(EV_REL), LABEL(EV_ABS), LABEL(EV_MSC), LABEL(EV_SW),
    LABEL(EV_LED), LABEL(EV_SND), LABEL(EV_REP), LABEL(EV_FF),  LABEL(EV_PWR), LABEL(EV_FF_STATUS),
    LABEL_END,
};
static struct label syn_labels[] = {
    LABEL_END,
};
static struct label key_labels[] = {
    LABEL_END,
};
static struct label rel_labels[] = {
    LABEL_END,
};
static struct label abs_labels[] = {
    LABEL_END,
};
static struct label sw_labels[] = {
    LABEL_END,
};
static struct label msc_labels[] = {
    LABEL_END,
};
static struct label led_labels[] = {
    LABEL_END,
};
static struct label rep_labels[] = {
    LABEL_END,
};
static struct label snd_labels[] = {
    LABEL_END,
};
static struct label mt_tool_labels[] = {
    LABEL_END,
};
static struct label ff_status_labels[] = {
    LABEL_END,
};
static struct label ff_labels[] = {
    LABEL_END,
};
