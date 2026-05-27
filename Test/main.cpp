#include "GeoCrs.h"

int main(int argc, char* argv[])
{
	GeoCrs crs1 = GeoCrs::FromUserInput("EPSG:4326");
	GeoCrs crs2;
	crs2.SetFromEpsg(4326);
	if (crs1 == crs2)
	{
		printf("CRS 1 and CRS 2 are the same.\n");
	}
	else
	{
		printf("CRS 1 and CRS 2 are different.\n");
	}
	return 0;
}

