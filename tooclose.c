#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <math.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>
#include "metar.h"
#include "datetoepoch.h"

// https://www.aviationweather.gov/docs/metar/stations.txt
static const char NearestMETAR[] = "KVNY"; // replace with closest METAR source

// Limits
static const double Horizontal_Separation = 2.0 / 3.0; // nautical miles
static const int32_t Vertical_Separation = 750; // feet
static const int32_t Speed_Minimum = 50; // both planes faster than in kts, filter out hovering TV helicopters
static const int32_t Altitude_Minimum = 600; // both planes higher than in feet, filter out local airport operations

#define PLANE_COUNT 1024 // never more than about 70 planes visible from the casa
#define RAW_STRING_LEN 256
#define CALLSIGN_LEN 16

#define DATA_STATS_DURATION (60 * 60) // report some stats every hour

static const char BotToken[] = "token.secret";

typedef struct plane_t {
	uint32_t valid;
	uint32_t icao;
	time_t last_seen;
	time_t last_speed;
	time_t last_location_time;
	char callsign[CALLSIGN_LEN];
	uint32_t latlong_valid;
	float latitude;
	float longitude;
        double lat_radians;
        double lon_radians;
	float prev_latitude;
	float prev_longitude;
	float prev_latitude_radians;
	float prev_longitude_radians;
	int32_t speed;
	int32_t altitude;
        int32_t reported;
        char msg3[RAW_STRING_LEN];
} plane_t;

typedef struct data_stats_t {
	uint32_t message_count;
	uint32_t max_plane_count;
	uint32_t flight_count;
	time_t next;
} data_stats_t;

static data_stats_t DataStats;

static int PlaneListCount;

static double
deg2rad(double d)
{
	double r;

	r = (d * M_PI) / 180.0;

	return r;
}

static double
rad2deg(double rad)
{
	return rad * 180.0 / M_PI;
}

static double
CalcDistance(double lat1, double lon1, double lat2, double lon2)
{
	double theta, dist;

	theta = lon1 - lon2;
	dist = sin(lat1) * sin(lat2) + cos(lat1) * cos(lat2) * cos(theta);
	dist = acos(dist);
	dist = rad2deg(dist);
	dist = dist * 60.0 * 1.1515 * 0.8684; // nautical miles https://www.geodatasource.com/developers/c

	return dist;
}

static void
ReportClosePlanes(plane_t *plane0, plane_t *plane1, double horiz_sep, int32_t verti_sep, int32_t time_sep)
{
        char *ch;
        char buffer[512];

        ch = ctime(&plane0->last_seen);
        if (ch)
        {
                strncpy(buffer, ch, sizeof(buffer) - 1);
                ch = strchr(buffer, '\n');
                if (ch)
                        *ch = 0;
        }
        else
                strcpy(buffer, "time problems");

	printf("0: %06X %s %.5f,%.5f %dft %dkts | 1: %06X %s %.5f,%.5f %dft %dkts | horiz: %2.3f, vert: %d, time: %s\n",
	       plane0->icao,
	       plane0->callsign,
               plane0->latitude,
               plane0->longitude,
               plane0->altitude,
               plane0->speed,

	       plane1->icao,
	       plane1->callsign,
               plane1->latitude,
               plane1->longitude,
               plane1->altitude,
               plane1->speed,

               horiz_sep,
               verti_sep,
               buffer);
        if ((ch = strchr(plane0->msg3, '\n')) != 0)
                *ch = '\0';
        if ((ch = strchr(plane1->msg3, '\n')) != 0)
                *ch = '\0';
        printf("\t%s\n\t%s\n", plane0->msg3, plane1->msg3);
        printf("\thttps://globe.adsb.fi/?icao=%x\n", plane0->icao);
        printf("\thttps://globe.adsb.fi/?icao=%x\n", plane1->icao);
}

static uint32_t
PlaneCheck(plane_t *plane)
{
        uint32_t valid_plane;

        valid_plane = plane->valid && ! plane->reported && plane->latlong_valid > 2 &&
                plane->speed >= Speed_Minimum && plane->altitude >= Altitude_Minimum;

        return valid_plane;
}

static void
DetectClosePlanes(plane_t planes[PLANE_COUNT])
{
	int32_t i, j;
        int32_t verti_sep;
        double horiz_sep;
        int32_t time_sep;

	for (i = 0; i < PlaneListCount - 1; ++i)
        {
		if (PlaneCheck(&planes[i]))
                        for (j = i + 1; j < PlaneListCount; ++j)
                        {
                                if (PlaneCheck(&planes[j]))
                                {
                                        horiz_sep = CalcDistance(planes[i].lat_radians, planes[i].lon_radians, planes[j].lat_radians, planes[j].lon_radians);
                                        verti_sep = labs(planes[i].altitude - planes[j].altitude);
                                        time_sep = labs(planes[i].last_location_time - planes[j].last_location_time);
                                        if (horiz_sep < Horizontal_Separation && verti_sep < Vertical_Separation && time_sep == 0)
                                        {
                                                ReportClosePlanes(&planes[i], &planes[j], horiz_sep, verti_sep, time_sep);
                                                ++planes[i].reported;
                                                ++planes[j].reported;
                                        }
                                }
                        }
        }
}

static plane_t *
InsertPlane(plane_t planes[PLANE_COUNT], uint32_t icao)
{
	int i;

	i = 0;
	while (i < PLANE_COUNT && planes[i].valid)
		++i;
	assert(i < PLANE_COUNT); // if this pops something incredibly strange is happening
	if (i > PlaneListCount)
		PlaneListCount = i;

	planes[i].valid = 1;
        planes[i].reported = 0;
	planes[i].icao = icao;
	planes[i].last_seen = 0;
	planes[i].last_speed = 0;
	planes[i].last_location_time = 0;
	strcpy(planes[i].callsign, "unknown ");
	planes[i].latlong_valid = 0;
	planes[i].speed = -1;
	planes[i].altitude = -100000;
        planes[i].latitude = 0;
        planes[i].longitude = 0;
        planes[i].lat_radians = 0;
        planes[i].lon_radians = 0;

	return &planes[i];
}

static plane_t *
FindPlane(plane_t planes[PLANE_COUNT], uint32_t icao)
{
	int i;
	plane_t *plane;

	i = 0;
	while (i < PlaneListCount && ! (planes[i].icao == icao && planes[i].valid))
		++i;
	if (i == PlaneListCount)
	{
		plane = InsertPlane(planes, icao);
		++DataStats.flight_count;
	}
	else
		plane = &planes[i];

	return plane;
}

static void
ProcessMSG3(char **pp, plane_t *plane, char raw_string[RAW_STRING_LEN])
{
	char *ch;
	int field;
	int32_t altitude;
	float lat, lon;
	double metar_temp_c, metar_elevation_m;
        double location_check;

	field = 0;
	while ((ch = strsep(pp, ",")) && field < 3)
		++field;
	if (ch == 0)
		return;
	altitude = strtol(ch, 0, 10);
	if (altitude < -500 || altitude > 100000)
		return;

	field = 0;
	while ((ch = strsep(pp, ",")) && field < 2)
		++field;
	if (ch == 0)
		return;

        lat = 1000.0;
	sscanf(ch, "%f", &lat);
        if (lat == 1000.0) // bad squiiter
                return;
	ch = strsep(pp, ",");
	if (ch == 0)
		return;
        lon = 1000.0;
	sscanf(ch, "%f", &lon);
        if (lon == 1000.0) // bad squitter
                return;
	
	plane->last_location_time = plane->last_seen;
	plane->altitude = altitude;
        if (plane->latlong_valid > 0)
        {
                plane->prev_latitude = plane->latitude;
                plane->prev_longitude = plane->longitude;
                plane->prev_latitude_radians = plane->lat_radians;
                plane->prev_longitude_radians = plane->lon_radians;
        }
	plane->latitude = lat;
	plane->longitude = lon;
        plane->lat_radians = deg2rad(lat);
        plane->lon_radians = deg2rad(lon);
	++plane->latlong_valid;
        if (plane->latlong_valid > 1)
        {
                location_check = CalcDistance(plane->lat_radians, plane->lon_radians, plane->prev_latitude_radians, plane->prev_longitude_radians);
                if (location_check > 3) // NM diff between location squitters
                        plane->latlong_valid = 0; // posible corrupted location data in squitter, start over
        }
        strncpy(plane->msg3, raw_string, RAW_STRING_LEN - 1);
	METARFetch(NearestMETAR, &metar_temp_c, &metar_elevation_m);
}

static void
ProcessMSG4(char **pp, plane_t *plane)
{
	char *ch;
	int field;
	int32_t speed;

	field = 0;
	while ((ch = strsep(pp, ",")) && field < 4)
		++field;
	if (ch == 0)
		return;
	
	speed = strtol(ch, 0, 10);
	if (speed <= 0 || speed > 3000)
		return;

	plane->last_speed = plane->last_seen;
	plane->speed = speed;
}

static void
ProcessMSG1(char **pp, plane_t *plane)
{
	char *ch;
	int field;

	field = 0;
	while ((ch = strsep(pp, ",")) && field < 2)
		++field;
	if (ch == 0 || *ch == '\0')
		return;
	strncpy(plane->callsign, ch, sizeof(plane->callsign) - 1);
}

static time_t
ProcessPlane(char **pp, plane_t planes[PLANE_COUNT], uint32_t message_id, uint32_t icao, char *raw_string)
{
	plane_t *plane;
	char *ch, *date_s, *time_s;
	time_t seen;

	ch = strsep(pp, ",");
	if (ch == 0 || *ch == '\0')
		return -1;
	date_s = strsep(pp, ",");
	if (date_s == 0 || *date_s == '\0')
		return -1;
	time_s = strsep(pp, ",");
	if (time_s == 0 || *time_s == '\0')
		return -1;
	seen = Date2Epoch(date_s, time_s);

	plane = FindPlane(planes, icao);
	plane->last_seen = seen;

	switch (message_id)
	{
	case 1 :
		ProcessMSG1(pp, plane);
		break;
	case 3 :
		ProcessMSG3(pp, plane, raw_string);
		break;
	case 4 :
		ProcessMSG4(pp, plane);
		break;
	}

	return seen;
}

static void
CleanPlanes(plane_t planes[PLANE_COUNT], time_t now, int enable_bot)
{
	int i, last_valid_plane;
	uint32_t plane_count;
	time_t duration;

	plane_count = 0;
	last_valid_plane = PlaneListCount - 1;
	for (i = 0; i < PlaneListCount; ++i)
	{
		if (planes[i].valid)
		{
			++plane_count;
			duration = now - planes[i].last_seen;
			if (duration > 10)
                        {
				planes[i].valid = 0;
				planes[i].latlong_valid = 0;
                        }
			else
				last_valid_plane = i;
		}
	}
	if (plane_count > DataStats.max_plane_count)
		DataStats.max_plane_count = plane_count;
	PlaneListCount = last_valid_plane + 1;
}

static void
ReportDataStats(plane_t planes[PLANE_COUNT])
{
	int i, len;
	time_t now;
	char buffer[256];

	now = time(0);
	if (DataStats.next > now)
		return;

	strcpy(buffer, ctime(&now));
	len = strlen(buffer);
	for (i = 0; i < len; ++i)
		if (buffer[i] == '\n')
			buffer[i] = '\0';
	printf("Hourly report %s:\n", buffer);
	printf("%25s: %.1f\n", "messages / sec", (double)DataStats.message_count / (double)DATA_STATS_DURATION);
	printf("%25s: %d\n", "max concurrent flights", DataStats.max_plane_count);
	printf("%25s: %d\n", "new flights", DataStats.flight_count);
	printf("%25s: %d\n", "plane list count", PlaneListCount);

	DataStats.message_count = 0;
	DataStats.max_plane_count = 0;
	DataStats.flight_count = 0;

	DataStats.next = now + DATA_STATS_DURATION;
}

int
main(int argc, char *argv[])
{
	int i, opt, enable_bot, usage;
	uint32_t message_id, icao;
	time_t seen, receiver_now;
	char buffer[1024], raw_string[256];
	char *p;
	char *ch;
	plane_t planes[PLANE_COUNT];

	enable_bot = 0;
	usage = 0;
	while ((opt = getopt(argc, argv, "b")) != EOF)
		switch (opt)
		{
		case 'b' :
			enable_bot = 1;
			break;
		default :
			usage = 1;
			break;
		}
	if (usage)
	{
		fprintf(stderr, "usage: %s [-b]\n", argv[0]);
		fprintf(stderr, "\t-b = enable bot reporting\n\n");
		fprintf(stderr, "\texample usage: nc localhost 30003 | %s\n", argv[0]);
		
		return 1;
	}

	if (enable_bot)
	{
		struct stat statbuf;
		
		if (stat(BotToken, &statbuf))
		{
			fprintf(stderr, "%s: cannot stat bot token file %s\n", argv[0], BotToken);
			exit(1);
		}
	}

	for (i = 0; i < PLANE_COUNT; ++i)
		planes[i].valid = 0;
	PlaneListCount = 0;
	DataStats.next = time(0) + DATA_STATS_DURATION;

	receiver_now = time(0);
	while (fgets(buffer, sizeof(buffer), stdin))
	{
		p = buffer;
                strncpy(raw_string, buffer, RAW_STRING_LEN - 1);
                raw_string[RAW_STRING_LEN - 1] = '\0';
		ch = strsep(&p, ",");
		if (ch && strncmp(ch, "MSG", 3) == 0)
		{
			++DataStats.message_count;
			ch = strsep(&p, ",");
			if (ch)
			{
				message_id = strtoul(ch, 0, 0);
				ch = strsep(&p, ",");
				if (ch)
				{
					ch = strsep(&p, ",");
					if (ch)
					{
						ch = strsep(&p, ",");
						icao = strtoul(ch, 0, 16);
						seen = ProcessPlane(&p, planes, message_id, icao, raw_string);
						if (seen != -1)
							receiver_now = seen;
					}
				}
			}
		}
		CleanPlanes(planes, receiver_now, enable_bot);
		DetectClosePlanes(planes);
		ReportDataStats(planes);
	}

	return 0;
}
