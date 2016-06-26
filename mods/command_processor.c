#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#define NSCORE 1
#include "nebcallbacks.h"
#include "nebmods.h"
#include "nebmodules.h"
#include "nebstructs.h"
#ifdef HAVE_ICINGA
#include "icinga.h"
#else
#include "nagios.h"
#endif
#include "broker.h"
#include "comments.h"
#include "common.h"
#include "downtime.h"
#include "objects.h"
#include <zmq.h>
#include "nagmq_common.h"

extern int errno;

const char* nagmq_source_name(const void* unused) {
    return "NagMQ";
}

struct check_engine nagmq_check_engine = {"NagMQ", nagmq_source_name, free};

static void process_status(json_t* payload) {
    char *host_name, *service_description = NULL, *output = NULL;
    check_result newcr;

    init_check_result(&newcr);
    newcr.output_file = NULL;
    newcr.output_file_fp = NULL;

    if (get_values(payload,
                   "host_name",
                   JSON_STRING,
                   1,
                   &host_name,
                   "service_description",
                   JSON_STRING,
                   0,
                   &service_description,
                   "output",
                   JSON_STRING,
                   1,
                   &output,
                   "return_code",
                   JSON_INTEGER,
                   1,
                   &newcr.return_code,
                   "start_time",
                   JSON_TIMEVAL,
                   0,
                   &newcr.start_time,
                   "finish_time",
                   JSON_TIMEVAL,
                   1,
                   &newcr.finish_time,
                   "check_type",
                   JSON_INTEGER,
                   1,
                   &newcr.check_type,
                   "check_options",
                   JSON_INTEGER,
                   0,
                   &newcr.check_options,
                   "scheduled_check",
                   JSON_INTEGER,
                   0,
                   &newcr.scheduled_check,
                   "reschedule_check",
                   JSON_INTEGER,
                   0,
                   &newcr.reschedule_check,
                   "latency",
                   JSON_REAL,
                   0,
                   &newcr.latency,
                   "early_timeout",
                   JSON_INTEGER,
                   0,
                   &newcr.early_timeout,
                   "exited_ok",
                   JSON_INTEGER,
                   0,
                   &newcr.exited_ok,
                   NULL) != 0) {
        logit(NSLOG_RUNTIME_WARNING, FALSE, "Invalid parameters in NagMQ check result");
        return;
    }

    service* service_target = NULL;
    if (service_description)
        service_target = find_service(host_name, service_description);
    host* host_target = find_host(host_name);
    if (host_target == NULL || (service_description && !service_target)) {
        logit(NSLOG_RUNTIME_WARNING, FALSE, "NagMQ received a check result for an invalid object");
        return;
    }

    newcr.host_name = strdup(host_name);
    if (service_target) {
        newcr.service_description = strdup(service_description);
        newcr.object_check_type = SERVICE_CHECK;
    }
    newcr.output = strdup(output);

    const char* debug_service_name = service_target ? newcr.service_description : "(N/A)";
    log_debug_info(DEBUGL_CHECKS,
                   DEBUGV_BASIC,
                   "Received a check result via NagMQ for %s %s\n",
                   newcr.host_name,
                   debug_service_name);

    newcr.engine = &nagmq_check_engine;
    process_check_result(&newcr);
    free_check_result(&newcr);
}

static void process_acknowledgement(json_t* payload) {
    char *host_name, *service_description = NULL, *author_name, *comment_data;
    int persistent_comment = 0, notify_contacts = 0, acknowledgement_type = 0, end_time = 0;
    host* host_target = NULL;
    service* service_target = NULL;
    json_error_t err;
    if (get_values(payload,
                   "host_name",
                   JSON_STRING,
                   1,
                   &host_name,
                   "service_description",
                   JSON_STRING,
                   0,
                   &service_description,
                   "author_name",
                   JSON_STRING,
                   1,
                   &author_name,
                   "comment_data",
                   JSON_STRING,
                   1,
                   &comment_data,
                   "acknowledgement_type",
                   JSON_INTEGER,
                   0,
                   &acknowledgement_type,
                   "notify_contacts",
                   JSON_TRUE,
                   0,
                   &notify_contacts,
                   "persistent_comment",
                   JSON_TRUE,
                   0,
                   &persistent_comment,
#ifdef HAVE_ICINGA
                   "end_time",
                   JSON_INTEGER,
                   0,
                   &end_time,
#endif
                   NULL) != 0) {
        logit(NSLOG_RUNTIME_WARNING,
              FALSE,
              "NagMQ received an acknowledgement with invalid parameters");
        return;
    }

    host_target = find_host(host_name);
    if (service_description)
        service_target = find_service(host_name, service_description);

#ifdef HAVE_ICINGA
    if (service_target)
        acknowledge_service_problem(service_target,
                                    author_name,
                                    comment_data,
                                    acknowledgement_type,
                                    notify_contacts,
                                    persistent_comment,
                                    end_time);
    else if (host_target)
        acknowledge_host_problem(host_target,
                                 author_name,
                                 comment_data,
                                 acknowledgement_type,
                                 notify_contacts,
                                 persistent_comment,
                                 end_time);
#else
    if (service_target)
        acknowledge_service_problem(service_target,
                                    author_name,
                                    comment_data,
                                    acknowledgement_type,
                                    notify_contacts,
                                    persistent_comment);
    else if (host_target)
        acknowledge_host_problem(host_target,
                                 author_name,
                                 comment_data,
                                 acknowledgement_type,
                                 notify_contacts,
                                 persistent_comment);
#endif

    log_debug_info(DEBUGL_EXTERNALCOMMANDS,
                   DEBUGV_BASIC,
                   "Received acknowledgement via NagMQ for %s %s\n",
                   host_name,
                   service_description ? service_description : "(n/a)");
}

static void process_comment(json_t* payload) {
    char *host_name, *service_description = NULL, *comment_data, *author_name;
    time_t expire_time = 0;
    int persistent = 0, expires = 0;
    struct timeval entry_time;
    if (get_values(payload,
                   "host_name",
                   JSON_STRING,
                   1,
                   &host_name,
                   "service_description",
                   JSON_STRING,
                   0,
                   &service_description,
                   "comment_data",
                   JSON_STRING,
                   1,
                   &comment_data,
                   "author_name",
                   JSON_STRING,
                   1,
                   &author_name,
                   "timestamp",
                   JSON_TIMEVAL,
                   1,
                   &entry_time,
                   "persistent",
                   JSON_TRUE,
                   1,
                   &persistent,
                   "expires",
                   JSON_TRUE,
                   1,
                   &expires,
                   "expire_time",
                   JSON_INTEGER,
                   0,
                   &expire_time,
                   NULL) != 0) {
        logit(NSLOG_RUNTIME_WARNING, FALSE, "NagMQ received a comment with invalid parameters");
        return;
    }

    add_new_comment((service_description == NULL) ? HOST_COMMENT : SERVICE_COMMENT,
                    USER_COMMENT,
                    host_name,
                    service_description,
                    entry_time.tv_sec,
                    author_name,
                    comment_data,
                    persistent,
                    COMMENTSOURCE_EXTERNAL,
                    expires,
                    expire_time,
                    NULL);
    log_debug_info(DEBUGL_EXTERNALCOMMANDS,
                   DEBUGV_BASIC,
                   "Received comment via NagMQ for %s %s\n",
                   host_name,
                   service_description ? service_description : "(n/a)");
}

static void process_downtime(json_t* payload) {
    char *host_name, *service_description = NULL;
    char *author_name = NULL, *comment_data = NULL;
    time_t start_time = 0, end_time = 0, entry_time = 0;
    int fixed;
    unsigned long duration = 0, triggered_by = 0, downtimeid;

    if (get_values(payload,
                   "host_name",
                   JSON_STRING,
                   1,
                   &host_name,
                   "service_description",
                   JSON_STRING,
                   0,
                   &service_description,
                   "entry_time",
                   JSON_INTEGER,
                   1,
                   &entry_time,
                   "author_name",
                   JSON_STRING,
                   0,
                   &author_name,
                   "comment_data",
                   JSON_STRING,
                   0,
                   &comment_data,
                   "start_time",
                   JSON_INTEGER,
                   1,
                   &start_time,
                   "end_time",
                   JSON_INTEGER,
                   1,
                   &end_time,
                   "fixed",
                   JSON_TRUE,
                   1,
                   &fixed,
                   "duration",
                   JSON_INTEGER,
                   1,
                   &duration,
                   "triggered_by",
                   JSON_INTEGER,
                   0,
                   &triggered_by,
                   NULL) != 0) {
        logit(NSLOG_RUNTIME_WARNING,
              FALSE,
              "NagMQ receieved a downtime request with invalid parameters");
        return;
    }

    schedule_downtime(service_description != NULL ? SERVICE_DOWNTIME : HOST_DOWNTIME,
                      host_name,
                      service_description,
                      entry_time,
                      author_name,
                      comment_data,
                      start_time,
                      end_time,
                      fixed,
                      triggered_by,
                      duration,
                      &downtimeid);
    log_debug_info(DEBUGL_EXTERNALCOMMANDS,
                   DEBUGV_BASIC,
                   "Received downtime via NagMQ for %s %s\n",
                   host_name,
                   service_description ? service_description : "(n/a)");
}

static void process_cmd(json_t* payload) {
    host* host_target = NULL;
    service* service_target = NULL;
    char *host_name = NULL, *service_description = NULL, *cmd_name;

    if (get_values(payload,
                   "host_name",
                   JSON_STRING,
                   1,
                   &host_name,
                   "service_description",
                   JSON_STRING,
                   0,
                   &service_description,
                   "command_name",
                   JSON_STRING,
                   1,
                   &cmd_name,
                   NULL) != 0) {
        logit(NSLOG_RUNTIME_WARNING, FALSE, "NagMQ received a command with invalid parameters");
        return;
    }

    log_debug_info(DEBUGL_EXTERNALCOMMANDS,
                   DEBUGV_BASIC,
                   "Received command %s via NagMQ for %s %s\n",
                   cmd_name,
                   host_name,
                   service_description ? service_description : "(n/a)");

    if (host_name) {
        host_target = find_host(host_name);
        if (host_target == NULL) {
            logit(NSLOG_RUNTIME_WARNING,
                  FALSE,
                  "NagMQ received command %s for host %s, but host isn't defined",
                  cmd_name,
                  host_name);
            return;
        }
    }
    if (host_target && service_description) {
        service_target = find_service(host_name, service_description);
        if (service_target == NULL) {
            logit(NSLOG_RUNTIME_WARNING,
                  FALSE,
                  "NagMQ received command %s for service %s %s, but service isn't defined",
                  cmd_name,
                  host_name,
                  service_description);
            return;
        }
    }

    if (strcmp(cmd_name, "disable_service_checks") == 0 && service_target)
        disable_service_checks(service_target);
    else if (strcmp(cmd_name, "enable_service_checks") == 0 && service_target)
        enable_service_checks(service_target);
    else if (strcmp(cmd_name, "enable_all_notifications") == 0)
        enable_all_notifications();
    else if (strcmp(cmd_name, "disable_all_notification") == 0)
        disable_all_notifications();
    else if (strcmp(cmd_name, "enable_service_notifications") == 0 && service_target)
        enable_service_notifications(service_target);
    else if (strcmp(cmd_name, "disable_service_notifications") == 0 && service_target)
        disable_service_notifications(service_target);
    else if (strcmp(cmd_name, "enable_host_notifications") == 0 && host_target)
        enable_host_notifications(host_target);
    else if (strcmp(cmd_name, "disable_host_notifications") == 0 && host_target)
        disable_host_notifications(host_target);
    else if (strcmp(cmd_name, "remove_host_acknowledgement") == 0 && host_target)
        remove_host_acknowledgement(host_target);
    else if (strcmp(cmd_name, "remove_service_acknowledgement") == 0 && service_target)
        remove_service_acknowledgement(service_target);
    else if (strcmp(cmd_name, "start_executing_service_checks") == 0)
        start_executing_service_checks();
    else if (strcmp(cmd_name, "stop_executing_service_checks") == 0)
        stop_executing_service_checks();
    else if (strcmp(cmd_name, "start_accepting_passive_service_checks") == 0)
        start_accepting_passive_service_checks();
    else if (strcmp(cmd_name, "stop_accepting_passive_service_checks") == 0)
        stop_accepting_passive_service_checks();
    else if (strcmp(cmd_name, "enable_passive_service_checks") == 0 && service_target)
        enable_passive_service_checks(service_target);
    else if (strcmp(cmd_name, "disable_passive_service_checks") == 0 && service_target)
        disable_passive_service_checks(service_target);
    else if (strcmp(cmd_name, "start_using_event_handlers") == 0)
        start_using_event_handlers();
    else if (strcmp(cmd_name, "stop_using_event_handlers") == 0)
        stop_using_event_handlers();
    else if (strcmp(cmd_name, "enable_service_event_handler") == 0 && service_target)
        enable_service_event_handler(service_target);
    else if (strcmp(cmd_name, "disable_service_event_handler") == 0 && service_target)
        disable_service_event_handler(service_target);
    else if (strcmp(cmd_name, "enable_host_event_handler") == 0 && host_target)
        enable_host_event_handler(host_target);
    else if (strcmp(cmd_name, "disable_host_event_handler") == 0 && host_target)
        disable_host_event_handler(host_target);
    else if (strcmp(cmd_name, "enable_host_checks") == 0 && host_target)
        enable_host_checks(host_target);
    else if (strcmp(cmd_name, "disable_host_checks") == 0 && host_target)
        disable_host_checks(host_target);
    else if (strcmp(cmd_name, "enable_service_freshness_checks") == 0)
        enable_service_freshness_checks();
    else if (strcmp(cmd_name, "start_obsessing_over_service") == 0 && service_target)
        start_obsessing_over_service(service_target);
    else if (strcmp(cmd_name, "stop_obsessing_over_service") == 0 && service_target)
        stop_obsessing_over_service(service_target);
    else if (strcmp(cmd_name, "start_obsessing_over_host") == 0 && host_target)
        start_obsessing_over_host(host_target);
    else if (strcmp(cmd_name, "stop_obsessing_over_host") == 0 && host_target)
        stop_obsessing_over_host(host_target);
    else if (strcmp(cmd_name, "enable_performance_data") == 0)
        enable_performance_data();
    else if (strcmp(cmd_name, "disable_performance_data") == 0)
        disable_performance_data();
    else if (strcmp(cmd_name, "start_executing_host_checks") == 0)
        start_executing_host_checks();
    else if (strcmp(cmd_name, "stop_executing_host_checks") == 0)
        stop_executing_host_checks();
    else if (strcmp(cmd_name, "start_accepting_passive_host_checks") == 0)
        start_accepting_passive_host_checks();
    else if (strcmp(cmd_name, "stop_accepting_passive_host_checks") == 0)
        stop_accepting_passive_host_checks();
    else if (strcmp(cmd_name, "enable_passive_host_checks") == 0 && host_target)
        enable_passive_host_checks(host_target);
    else if (strcmp(cmd_name, "disable_passive_host_checks") == 0 && host_target)
        disable_passive_host_checks(host_target);
    else if (strcmp(cmd_name, "enable_host_flap_detection") == 0 && host_target)
        enable_host_flap_detection(host_target);
    else if (strcmp(cmd_name, "disable_host_flap_detection") == 0 && host_target)
        disable_host_flap_detection(host_target);
    else if (strcmp(cmd_name, "enable_service_flap_detection") == 0 && service_target)
        enable_service_flap_detection(service_target);
    else if (strcmp(cmd_name, "disable_service_flap_detection") == 0 && service_target)
        disable_service_flap_detection(service_target);
    else if ((strcmp(cmd_name, "schedule_host_check") == 0 && host_target) ||
             (strcmp(cmd_name, "schedule_service_check") == 0 && service_target)) {
        time_t next_check;
        int force_execution = 0, freshness_check = 0, orphan_check = 0;
        if (get_values(payload,
                       "next_check",
                       JSON_INTEGER,
                       1,
                       &next_check,
                       "force_execution",
                       JSON_TRUE,
                       0,
                       &force_execution,
                       "freshness_check",
                       JSON_TRUE,
                       0,
                       &freshness_check,
                       "orphan_check",
                       JSON_TRUE,
                       0,
                       &orphan_check,
                       NULL) != 0) {
            logit(
                NSLOG_RUNTIME_WARNING,
                FALSE,
                "Received a %s request via NagMQ for %s %s, but a parameter was missing or invalid",
                cmd_name,
                host_name,
                service_description ? service_description : "(n/a)");
            return;
        }
        int flags = CHECK_OPTION_NONE;
        if (force_execution)
            flags |= CHECK_OPTION_FORCE_EXECUTION;
        if (freshness_check)
            flags |= CHECK_OPTION_FRESHNESS_CHECK;
        if (orphan_check)
            flags |= CHECK_OPTION_ORPHAN_CHECK;
        if (service_target)
            schedule_service_check(service_target, next_check, flags);
        else
            schedule_host_check(host_target, next_check, flags);
    } else if ((strcmp(cmd_name, "disable_and_propagate_notifications") == 0 ||
                strcmp(cmd_name, "enable_and_propagate_notifications") == 0) &&
               host_target) {
        int affect_top_host = 0, affect_hosts = 0, affect_services = 0, level = 0;
        if (get_values(payload,
                       "affect_top_host",
                       JSON_TRUE,
                       0,
                       &affect_top_host,
                       "affect_hosts",
                       JSON_TRUE,
                       0,
                       &affect_hosts,
                       "affect_services",
                       JSON_TRUE,
                       0,
                       &affect_services,
                       "level",
                       JSON_INTEGER,
                       0,
                       &level,
                       NULL) != 0) {
            logit(
                NSLOG_RUNTIME_WARNING,
                FALSE,
                "Received a %s request via NagMQ for %s %s, but a parameter was missing or invalid",
                cmd_name,
                host_name,
                service_description ? service_description : "(n/a)");
            return;
        }
        if (strcmp(cmd_name, "disable_and_propagate_notifications") == 0)
            disable_and_propagate_notifications(
                host_target, level, affect_top_host, affect_hosts, affect_services);
        else if (strcmp(cmd_name, "enable_and_propagate_notifications") == 0)
            enable_and_propagate_notifications(
                host_target, level, affect_top_host, affect_hosts, affect_services);
    }
#ifdef HAVE_DELETE_DOWNTIME_LONGNAME
    else if (strcmp(cmd_name, "delete_downtime") == 0) {
        char* comment = NULL;
        time_t start_time = 0;
        get_values(payload,
                   "comment",
                   JSON_STRING,
                   0,
                   &comment,
                   "start_time",
                   JSON_INTEGER,
                   0,
                   &start_time,
                   NULL);
        delete_downtime_by_hostname_service_description_start_time_comment(
            host_name, service_description, start_time, comment);
    }
#endif
}

void process_pull_msg(zmq_msg_t* payload_msg) {
    char* type = NULL;
    json_error_t errobj;

    // Silently discard any empty messages.
    if (zmq_msg_size(payload_msg) == 0)
        return;

    json_t* payload = json_loadb(zmq_msg_data(payload_msg), zmq_msg_size(payload_msg), 0, &errobj);
    if (payload == NULL) {
        logit(NSLOG_RUNTIME_WARNING,
              FALSE,
              "NagMQ received a command, but it wasn't valid JSON. %s at position %d",
              errobj.text,
              errobj.position);
        return;
    }

    if (get_values(payload, "type", JSON_STRING, 1, &type, NULL) != 0) {
        logit(NSLOG_RUNTIME_WARNING,
              FALSE,
              "NagMQ received an invalid command - it had no type field");
        json_decref(payload);
        return;
    }

    if (strcmp(type, "command") == 0)
        process_cmd(payload);
    else if (strcmp(type, "host_check_processed") == 0 ||
             strcmp(type, "service_check_processed") == 0)
        process_status(payload);
    else if (strcmp(type, "acknowledgement") == 0)
        process_acknowledgement(payload);
    else if (strcmp(type, "comment_add") == 0)
        process_comment(payload);
    else if (strcmp(type, "downtime_add") == 0)
        process_downtime(payload);
    json_decref(payload);
    return;
}