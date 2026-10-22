#include "usage_stats.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

static SessionSummary session(uint64_t start,uint64_t duration,SessionKind kind=SessionKind::Mirror) {
    SessionSummary s;s.started_utc=start;s.duration_ms=duration;s.kind=kind;s.width=kind==SessionKind::Audio?0:1920;s.height=kind==SessionKind::Audio?0:1080;s.codec=kind==SessionKind::Audio?0:2;return s;
}
int main() {
    char dir[]="/tmp/uxair-usage-test-XXXXXX";assert(mkdtemp(dir));
    const std::string root(dir),path=root+"/usage-v1";
    UsageStats empty;assert(!load_usage_stats(path,empty)&&!empty.storage_corrupt);
    assert(summarize_usage(empty,UsagePeriod::Days7,20000).bars.size()==7);
    assert(usage_axis_label(12500)=="12.5 s");
    assert(usage_axis_label(200)=="0.2 s");
    assert(usage_axis_label(900000)=="15 min");
    assert(usage_axis_label(5400000)=="1.5 h");
    assert(usage_mirror_percent(0,0)==0);
    assert(usage_mirror_percent(1,2)==33&&usage_mirror_percent(2,1)==67);
    const unsigned mirror_pct=usage_mirror_percent(1234567,7654321);
    assert(mirror_pct+(100-mirror_pct)==100);

    assert(usage_axis_label(0)=="0 s");
    assert(usage_axis_label(60000)=="1 min");
    assert(usage_axis_label(3600000)=="1 h");
    assert(usage_axis_label(0,60000)=="0 min");
    // Every scale covers its data, including former floor-rounding failures.
    for(uint64_t value:{0ULL,1ULL,1000ULL,10000ULL,59000ULL,60000ULL,90000ULL,151000ULL,
                       1800000ULL,10800000ULL,99999ULL,100000ULL,100001ULL,199999ULL,
                       200000ULL,200001ULL,500001ULL,UINT64_MAX-1ULL,static_cast<unsigned long long>(UINT64_MAX)}) {
        const uint64_t axis=usage_chart_axis(value);assert(axis>=value&&axis>0);
        for(int height:{0,1,83,200}) {
            auto h=usage_bar_heights(value/3,value-value/3,axis,height);
            assert(h.mirror>=0&&h.audio>=0&&h.mirror+h.audio<=height);
            if(value&&height)assert(h.mirror+h.audio>0);
        }
    }
    assert(usage_chart_axis(151000)==240000);
    auto exact=usage_bar_heights(120000,80000,200000,100);assert(exact.mirror==60&&exact.audio==40);
    auto clipped=usage_bar_heights(UINT64_MAX,UINT64_MAX,1,100);assert(clipped.mirror+clipped.audio==100);
    auto day=[](int year,int month,int date) {struct tm t={};t.tm_year=year-1900;t.tm_mon=month-1;t.tm_mday=date;return usage_utc_day(uint64_t(timegm(&t)));};
    const uint32_t oct21=day(2026,10,21);
    UsageStats calendar;calendar.daily_from_day=day(2026,10,10);
    DailyUsage sample;sample.utc_day=oct21;sample.mirror_ms=151000;sample.audio_ms=59000;calendar.daily.push_back(sample);
    // Future stored records are ignored, including clock-skewed data in the current month.
    sample.utc_day=day(2026,10,22);sample.mirror_ms=999999;calendar.daily.push_back(sample);
    auto october=summarize_usage(calendar,UsagePeriod::ThisMonth,oct21);
    assert(october.title=="This month — October 2026"&&october.bars.size()==31);
    assert(october.bars.front().label=="1"&&october.bars.back().label=="31");
    assert(october.bars[20].current&&october.bars[20].mirror_ms==151000);
    assert(!october.bars[8].available&&!october.bars[8].future);
    assert(october.bars[9].available&&october.bars[9].mirror_ms==0);
    assert(october.bars[21].future&&!october.bars[21].available&&october.bars[21].mirror_ms==0);
    auto annual=summarize_usage(calendar,UsagePeriod::ThisYear,oct21);
    assert(annual.title=="This year — 2026"&&annual.bars.size()==12);
    assert(annual.bars[0].label=="Jan"&&annual.bars[11].label=="Dec");
    assert(annual.bars[9].label=="Oct"&&annual.bars[9].current&&annual.bars[9].partial&&annual.bars[9].mirror_ms==151000);
    assert(!annual.bars[8].available&&!annual.bars[8].future&&annual.bars[10].future);
    calendar.daily_from_day=day(2026,1,1);
    annual=summarize_usage(calendar,UsagePeriod::ThisYear,oct21);
    assert(annual.bars[9].current&&!annual.bars[9].partial); // complete coverage to date
    assert(annual.bars[0].available&&!annual.bars[0].partial);
    for(int year:{2024,2025,2026,2100,2000})for(int month=1;month<=12;++month) {
        // Keep representable dates for the ARMv7 time_t convention; Gregorian century rules tested on host.
        auto result=summarize_usage(calendar,UsagePeriod::ThisMonth,day(year,month,1));
        const bool leap=(year%4==0&&(year%100!=0||year%400==0));
        const int expected=month==2?(leap?29:28):(month==4||month==6||month==9||month==11)?30:31;
        assert(result.bars.size()==size_t(expected));assert(result.bars[0].current&&result.bars[1].future);
    }
    auto january=summarize_usage(calendar,UsagePeriod::ThisYear,day(2027,1,1));
    assert(january.bars[0].current&&january.bars[1].future&&!january.bars[0].partial);
    // Jan 1 of a leap year is outside the last 365 UTC dates on Dec 31.
    calendar.daily_from_day=day(2024,1,1);
    auto retained=summarize_usage(calendar,UsagePeriod::ThisYear,day(2024,12,31));
    assert(retained.bars[0].partial);
    auto retained_days=summarize_usage(calendar,UsagePeriod::ThisMonth,day(2024,1,1));
    assert(retained_days.bars[0].available);
    auto no_history=summarize_usage(empty,UsagePeriod::ThisYear,oct21);
    assert(!no_history.bars[9].available&&!no_history.bars[9].future&&no_history.bars[10].future);

    // A session crossing UTC new year contributes to both daily buckets.
    UsageStats cross;cross.daily_from_day=usage_utc_day(1704067190);
    cross.record(session(1704067190,20000));
    assert(cross.daily.size()==2);
    assert(cross.daily[0].utc_day+1==cross.daily[1].utc_day);
    assert(cross.daily[0].mirror_ms==10000&&cross.daily[1].mirror_ms==10000);
    assert(cross.daily[0].mirror_sessions==1&&cross.daily[1].mirror_sessions==1);
    UsageSummary boundary=summarize_usage(cross,UsagePeriod::Months12,usage_utc_day(1704067200));
    bool saw_dec=false,saw_jan=false;
    for(const auto& bar:boundary.bars) {
        if(bar.label=="Dec") {saw_dec=true;assert(bar.available&&bar.partial&&bar.mirror_ms==10000);}
        if(bar.label=="Jan") {saw_jan=true;assert(bar.available&&bar.partial&&bar.mirror_ms==10000);}
    }
    assert(saw_dec&&saw_jan);

    // Recent history is bounded at 100 while lifetime counters continue.
    UsageStats many;many.daily_from_day=usage_utc_day(1700000000);
    for(unsigned i=0;i<105;++i)many.record(session(1700000000+i*86400,60000,i%3?SessionKind::Mirror:SessionKind::Audio));
    assert(many.successful_sessions==105&&many.recent.size()==100);
    assert(many.recent.front().started_utc==1700000000+104*86400);
    assert(many.recent.back().started_utc==1700000000+5*86400);
    assert(many.daily.size()<=UsageStats::MaxDailyRecords);
    assert(save_usage_stats(path,many));
    assert(usage_storage_bytes(path)>0&&usage_storage_bytes(path)<200*1024);
    UsageStats restored;assert(load_usage_stats(path,restored));
    assert(restored.successful_sessions==105&&restored.recent.size()==100);
    UsageSummary week=summarize_usage(restored,UsagePeriod::Days7,usage_utc_day(1700000000+104*86400));
    assert(week.bars.size()==7&&week.mirror_ms+week.audio_ms>0);
    UsageSummary thirty=summarize_usage(restored,UsagePeriod::Days30,usage_utc_day(1700000000+104*86400));
    assert(thirty.bars.size()==30);
    UsageSummary months=summarize_usage(restored,UsagePeriod::Months12,usage_utc_day(1700000000+104*86400));
    assert(months.bars.size()==12);
    uint32_t leap_day=usage_utc_day(1709164800); // 2024-02-29 UTC
    UsageStats leap;leap.daily_from_day=leap_day;
    DailyUsage leap_record;leap_record.utc_day=leap_day;leap_record.mirror_ms=86400000;leap.daily.push_back(leap_record);
    UsageSummary leap_month=summarize_usage(leap,UsagePeriod::Months12,usage_utc_day(1709251200)); // 2024-03-01
    bool found_february=false;for(const auto& bar:leap_month.bars)if(bar.label=="Feb"){found_february=true;assert(bar.available&&bar.partial&&bar.mirror_ms==86400000);}
    assert(found_february&&leap_month.bars.back().label=="Mar");
    UsageSummary leap_week=summarize_usage(leap,UsagePeriod::Days7,usage_utc_day(1709251200));
    assert(leap_week.bars.back().available&&leap_week.bars.back().mirror_ms==0); // zero-use current day is distinct from unavailable dates

    // V1 lifetime totals and recent entries migrate without inventing chart data.
    {std::ofstream v1(path.c_str());v1<<"UXPLAY_USAGE_V1\n"<<"totals 9000 2 1 1 0 90 80 1 2 3 4\n"
      <<"session 1700000000 5000 1 1280 720 1 30 29 0 0 0 0\n"
      <<"session 1700000100 4000 2 0 0 0 0 0 0 0 0 0\n";}
    UsageStats migrated;assert(load_usage_stats(path,migrated));
    assert(migrated.successful_sessions==2&&migrated.playback_ms==9000&&migrated.recent.size()==2);
    assert(migrated.daily.empty()&&migrated.daily_from_day==usage_utc_day((uint64_t)time(nullptr)));
    migrated.recent.push_front(session(1700000000,1000));migrated.recent.front().width=2160;migrated.recent.front().height=3840;
    assert(save_usage_stats(path,migrated));UsageStats portrait;assert(load_usage_stats(path,portrait));
    assert(portrait.recent.front().width==2160&&portrait.recent.front().height==3840);
    assert(save_usage_stats(path,migrated));
    {std::ifstream v2(path.c_str());std::string header;std::getline(v2,header);assert(header=="UXAIR_USAGE_V3");}

    UsageStats direct;direct.daily_from_day=usage_utc_day(1700000000);
    direct.record(session(1700000000,15000,SessionKind::Direct));
    assert(direct.daily.size()==1&&direct.daily[0].direct_ms==15000&&direct.daily[0].mirror_ms==0);
    assert(save_usage_stats(path,direct));UsageStats direct_loaded;assert(load_usage_stats(path,direct_loaded));
    auto direct_summary=summarize_usage(direct_loaded,UsagePeriod::Days7,usage_utc_day(1700000000));
    assert(direct_summary.direct_ms==15000&&direct_loaded.direct_sessions==1);
    {std::ofstream legacy(path.c_str());legacy<<"UXAIR_USAGE_V2\ntotals 0 0 0 0 0 0 0 0 0 0 0\ncoverage 19675\nsessions 0\ndaily 1\nday 19675 1000 2000 1 1\n";}
    UsageStats old;assert(load_usage_stats(path,old)&&old.daily[0].direct_ms==0&&old.daily[0].mirror_ms==1000);
    // Invalid and oversized files are bounded, preserved, and require explicit reset.
    {std::ofstream bad(path.c_str(),std::ios::binary);bad<<"UXAIR_USAGE_V2\ntotals 1";}
    UsageStats corrupt;assert(!load_usage_stats(path,corrupt)&&corrupt.storage_corrupt);
    assert(!save_usage_stats(path,corrupt));
    {std::ofstream big(path.c_str(),std::ios::binary);std::string bytes(UsageStats::MaxSerializedBytes+1,'x');big.write(bytes.data(),bytes.size());}
    UsageStats oversized;assert(!load_usage_stats(path,oversized)&&oversized.storage_corrupt);
    oversized.reset();assert(save_usage_stats(path,oversized));
    assert(usage_storage_bytes(path)<UsageStats::MaxSerializedBytes);

    // Reset does not mutate an in-memory active session; it may be recorded normally at finish.
    UsageStats during;during.record(session(1700000000,60000));during.reset();
    assert(during.successful_sessions==0&&during.recent.empty());
    during.record(session(1700000060,30000));assert(during.successful_sessions==1&&during.recent.size()==1);
    remove(path.c_str());rmdir(root.c_str());
    std::puts("PASS: bounded usage history, UTC aggregation, V1 migration, periods, corruption and reset");
}
