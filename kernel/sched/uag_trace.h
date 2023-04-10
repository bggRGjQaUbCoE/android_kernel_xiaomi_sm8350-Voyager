/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM uad

#if !defined(_TRACE_UAG_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_UAG_H
#include <linux/string.h>
#include <linux/types.h>
#include <linux/tracepoint.h>

#ifdef CONFIG_OPLUS_UAG_AMU_AWARE
#include "stall_util_cal.h"

DECLARE_PER_CPU(struct amu_data, amu_delta);
DECLARE_PER_CPU(u64, amu_update_delta_time);
DECLARE_PER_CPU(u64, amu_normal_util);
DECLARE_PER_CPU(u64, amu_stall_util);

TRACE_EVENT(uag_update_amu_counter,
	    TP_PROTO(int cpu, u64 time),
	    TP_ARGS(cpu, time),
	    TP_STRUCT__entry(
		    __field(int, cpu)
		    __field(u64, time)
		    __field(u64, delta_0)
		    __field(u64, delta_1)
		    __field(u64, delta_2)
		    __field(u64, delta_3)
		    __field(u64, delta_time)
		    __field(u64, normal_util)
		    __field(u64, stall_util)),
	    TP_fast_assign(
		    __entry->cpu = cpu;
		    __entry->time = time;
		    __entry->delta_0 = per_cpu(amu_delta, cpu).val[0];
		    __entry->delta_1 = per_cpu(amu_delta, cpu).val[1];
		    __entry->delta_2 = per_cpu(amu_delta, cpu).val[2];
		    __entry->delta_3 = per_cpu(amu_delta, cpu).val[3];
		    __entry->delta_time = per_cpu(amu_update_delta_time, cpu);
		    __entry->normal_util = per_cpu(amu_normal_util, cpu);
		    __entry->stall_util = per_cpu(amu_stall_util, cpu);),
	    TP_printk("cpu=%d delta_cntr=%llu,%llu,%llu,%llu delta_time=%llu util=%llu,%llu time=%llu",
		    __entry->cpu,
		    __entry->delta_0,
		    __entry->delta_1,
		    __entry->delta_2,
		    __entry->delta_3,
		    __entry->delta_time,
		    __entry->normal_util,
		    __entry->stall_util,
		    __entry->time)
);

TRACE_EVENT(uag_amu_adjust_util,
	    TP_PROTO(int cpu,
		    u64 orig, u64 normal, u64 stall, u64 reduce_pct,
		    u64 amu_result, u64 final_util, int policy),
	    TP_ARGS(cpu, orig, normal, stall, reduce_pct,
		    amu_result, final_util, policy),
	    TP_STRUCT__entry(
		    __field(int, cpu)
		    __field(u64, orig)
		    __field(u64, normal)
		    __field(u64, stall)
		    __field(u64, reduce_pct)
		    __field(u64, amu_result)
		    __field(u64, final_util)
		    __field(int, policy)),
	    TP_fast_assign(
		    __entry->cpu = cpu;
		    __entry->orig = orig;
		    __entry->normal = normal;
		    __entry->stall = stall;
		    __entry->reduce_pct = reduce_pct;
		    __entry->amu_result = amu_result;
		    __entry->final_util = final_util;
		    __entry->policy = policy;),
	    TP_printk("cpu=%d orig=%llu normal=%llu stall=%llu reduce_pct=%llu amu_result=%llu final_util=%llu report_policy=%d",
		    __entry->cpu,
		    __entry->orig,
		    __entry->normal,
		    __entry->stall,
		    __entry->reduce_pct,
		    __entry->amu_result,
		    __entry->final_util,
		    __entry->policy)
);
#endif /* CONFIG_OPLUS_UAG_AMU_AWARE */
#endif /* _TRACE_UAG_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE uag_trace
/* This part must be outside protection */
#include <trace/define_trace.h>
