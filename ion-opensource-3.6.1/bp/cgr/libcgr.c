/*
	libcgr.c:	functions implementing Contact Graph Routing.

	Author: Scott Burleigh, JPL

	Adaptation to use Dijkstra's Algorithm developed by John
	Segui, 2011.

	Adaptation for Earliest Transmission Opportunity developed
	by N. Bezirgiannidis and V. Tsaoussidis, Democritus University
	of Thrace, 2014.

	Adaptation for Overbooking management developed by C. Caini,
	D. Padalino, and M. Ruggieri, University of Bologna, 2014.

	Copyright (c) 2008, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
									*/
#include "cgr.h"

#define	MAX_TIME	((unsigned int) ((1U << 31) - 1))

#define	CGRVDB_NAME	"cgrvdb"

#ifdef	ION_BANDWIDTH_RESERVED
#define	MANAGE_OVERBOOKING	0
#endif

#ifndef	MANAGE_OVERBOOKING
#define	MANAGE_OVERBOOKING	1
#endif

/*		Perform a trace if a trace callback exists.		*/
#define TRACE(...) do \
{ \
	if (trace) \
	{ \
		trace->fn(trace->data, __LINE__, __VA_ARGS__); \
	} \
} while (0)

#define	PAYLOAD_CLASSES		3

/*		CGR-specific RFX data structures.			*/

typedef struct
{
	/*	Contact that forms the initial hop of the route.	*/

	uvast		toNodeNbr;	/*	Initial-hop neighbor.	*/
	time_t		fromTime;	/*	As from time(2).	*/

	/*	Time at which route shuts down: earliest contact
	 *	end time among all contacts in the end-to-end path.	*/

	time_t		toTime;		/*	As from time(2).	*/

	/*	Details of the route.					*/

	float		arrivalConfidence;
	time_t		arrivalTime;	/*	As from time(2).	*/
	PsmAddress	hops;		/*	SM list: IonCXref addr	*/
	uvast		maxCapacity;
} CgrRoute;		/*	IonNode routingObject is list of these.	*/

typedef struct
{
	/*	Working values, reset for each Dijkstra run.		*/

	IonCXref	*predecessor;	/*	On path to destination.	*/
	uvast		capacity;
	time_t		arrivalTime;	/*	As from time(2).	*/
	int		visited;	/*	Boolean.		*/
	int		suppressed;	/*	Boolean.		*/
} CgrContactNote;	/*	IonCXref routingObject is one of these.	*/

/*		Data structure for the CGR volatile database.		*/

typedef struct
{
	struct timeval	lastLoadTime;	/*	Add/del contacts/ranges	*/

	/*	There is one entry in the routeLists list for each
	 *	remote destination node.
	 *
	 *	The content of each routeLists list entry is, itself,
	 *	the address of an SmList.
	 *
	 *	The *list user data* of the SmList at that address is
	 *	the address of the IonNode structure for the remote
	 *	destination node.
	 *
	 *	The *entries* of the SmList at that address contain
	 *	the addresses of CgrRoute structures.
	 *
	 *	But the number of CgrRoutes in the route list for
	 *	any single remote destination node is limited:
	 *	there can be at most one for each neighbor of the
	 *	local node.						*/

	PsmAddress	routeLists;	/*	SM list: CgrRoute list	*/
} CgrVdb;

/*		Data structure for temporary linked list.		*/

typedef struct
{
	uvast		neighborNodeNbr;
	time_t		forfeitTime;
	float		arrivalConfidence;
	time_t		arrivalTime;	/*	As from time(2).	*/
	uvast		capacity;	/*	# bytes conveyed.	*/
	int		hopCount;	/*	# hops from dest. node.	*/
	Scalar		overbooked;	/*	Bytes needing reforward.*/
	Scalar		protected;	/*	Bytes not overbooked.	*/
} ProximateNode;

/*		Functions for managing the CGR database.		*/

static void	discardRouteList(PsmPartition ionwm, PsmAddress routes)
{
	PsmAddress	elt2;
	PsmAddress	next2;
	PsmAddress	addr;
	CgrRoute	*route;

	/*	Destroy all routes computed for one remote destination
	 *	node.							*/

	if (routes == 0)
	{
		return;	/*	This node has no routes to destroy.	*/
	}

	/*	Erase all routes in the list.				*/

	for (elt2 = sm_list_first(ionwm, routes); elt2; elt2 = next2)
	{
		next2 = sm_list_next(ionwm, elt2);
		addr = sm_list_data(ionwm, elt2);
		route = (CgrRoute *) psp(ionwm, addr);
		if (route->hops)
		{
			sm_list_destroy(ionwm, route->hops, NULL, NULL);
		}

		psm_free(ionwm, addr);
		sm_list_delete(ionwm, elt2, NULL, NULL);
	}

	/*	Destroy the list itself.				*/

	sm_list_destroy(ionwm, routes, NULL, NULL);
}

static void	discardRouteLists(CgrVdb *vdb)
{
	PsmPartition	ionwm = getIonwm();
	PsmAddress	elt;
	PsmAddress	nextElt;
	PsmAddress	routes;		/*	SM list: CgrRoute	*/
	PsmAddress	addr;
	IonNode		*node;

	/*	Destroy all route lists.				*/

	for (elt = sm_list_first(ionwm, vdb->routeLists); elt; elt = nextElt)
	{
		nextElt = sm_list_next(ionwm, elt);
		routes = sm_list_data(ionwm, elt);	/*	SmList	*/

		/*	Detach route list from remote node.		*/

		addr = sm_list_user_data(ionwm, routes);
		node = (IonNode *) psp(ionwm, addr);
		node->routingObject = 0;

		/*	Discard the list of routes to remote node.	*/

		discardRouteList(ionwm, routes);

		/*	And delete the reference to the destroyed list.	*/

		sm_list_delete(ionwm, elt, NULL, NULL);
	}
}

static void	clearRoutingObjects(PsmPartition ionwm)
{
	IonVdb		*ionvdb = getIonVdb();
	PsmAddress	elt;
	IonNode		*node;
	PsmAddress	routes;

	/*	Discard all routes computed for all destination
	 *	nodes, whether or not in route list (i.e., reachable
	 *	from CGR vdb.						*/

	for (elt = sm_rbt_first(ionwm, ionvdb->nodes); elt;
			elt = sm_rbt_next(ionwm, elt))
	{
		node = (IonNode *) psp(ionwm, sm_rbt_data(ionwm, elt));
		if (node->routingObject)
		{
			routes = node->routingObject;
			node->routingObject = 0;
			discardRouteList(ionwm, routes);
		}
	}
}

static CgrVdb	*getCgrVdb()
{
	static char	*name = CGRVDB_NAME;
	PsmPartition	ionwm = getIonwm();
	PsmAddress	vdbAddress;
	PsmAddress	elt;
	CgrVdb		*vdb;
	Sdr		sdr;

	/*	Attaching to volatile database.				*/

	if (psm_locate(ionwm, name, &vdbAddress, &elt) < 0)
	{
		putErrmsg("Failed searching for vdb.", name);
		return NULL;
	}

	if (elt)
	{
		vdb = (CgrVdb *) psp(ionwm, vdbAddress);
		return vdb;
	}

	/*	CGR volatile database doesn't exist yet.		*/

	sdr = getIonsdr();
	CHKNULL(sdr_begin_xn(sdr));	/*	To lock memory.		*/
	vdbAddress = psm_zalloc(ionwm, sizeof(CgrVdb));
	if (vdbAddress == 0)
	{
		sdr_exit_xn(sdr);
		putErrmsg("No space for CGR volatile database.", name);
		return NULL;
	}

	vdb = (CgrVdb *) psp(ionwm, vdbAddress);
	memset((char *) vdb, 0, sizeof(CgrVdb));
	if ((vdb->routeLists = sm_list_create(ionwm)) == 0
	|| psm_catlg(ionwm, name, vdbAddress) < 0)
	{
		sdr_exit_xn(sdr);
		putErrmsg("Can't initialize CGR volatile database.", name);
		return NULL;
	}

	clearRoutingObjects(ionwm);
	sdr_exit_xn(sdr);
	return vdb;
}

/*		Functions for populating the routing table.		*/

static int	getApplicableRange(IonCXref *contact, unsigned int *owlt)
{
	PsmPartition	ionwm = getIonwm();
	IonVdb		*ionvdb = getIonVdb();
	IonRXref	arg;
	PsmAddress	elt;
	IonRXref	*range;

	*owlt = 0;		/*	Default.			*/
	if (contact->discovered)
	{
		return 0;	/*	Physically adjacent nodes.	*/
	}

	/*	This is a scheduled contact; need to know the OWLT.	*/

	memset((char *) &arg, 0, sizeof(IonRXref));
	arg.fromNode = contact->fromNode;
	arg.toNode = contact->toNode;
	for (oK(sm_rbt_search(ionwm, ionvdb->rangeIndex, rfx_order_ranges,
			&arg, &elt)); elt; elt = sm_rbt_next(ionwm, elt))
	{
		range = (IonRXref *) psp(ionwm, sm_rbt_data(ionwm, elt));
		CHKERR(range);
		if (range->fromNode > arg.fromNode
		|| range->toNode > arg.toNode)
		{
			break;
		}

		if (range->toTime < contact->fromTime)
		{
			continue;	/*	Range is in the past.	*/
		}

		if (range->fromTime > contact->fromTime)
		{
			break;
		}

		/*	Found applicable range.				*/

		*owlt = range->owlt;
		return 0;
	}

	/*	No applicable range.					*/

	return -1;
}

static int	computeDistanceToTerminus(IonCXref *rootContact,
			CgrContactNote *rootWork, IonNode *terminusNode,
			CgrRoute *route, CgrTrace *trace)
{
	PsmPartition	ionwm = getIonwm();
	IonVdb		*ionvdb = getIonVdb();
	IonCXref	*current;
	CgrContactNote	*currentWork;
	IonCXref	arg;
	PsmAddress	elt;
	IonCXref	*contact;
	CgrContactNote	*work;
	unsigned int	owlt;
	unsigned int	owltMargin;
	time_t		transmitTime;
	time_t		arrivalTime;
	IonCXref	*finalContact = NULL;
	time_t		earliestFinalArrivalTime = MAX_TIME;
	IonCXref	*nextContact;
	time_t		earliestArrivalTime;
	uvast		largestCapacity;
	time_t		earliestStartTime;
	time_t		earliestEndTime;
	uvast		maxCapacity;
	PsmAddress	addr;

	/*	This is an implementation of Dijkstra's Algorithm.	*/

	TRACE(CgrBeginRoute);
	current = rootContact;
	currentWork = rootWork;
	memset((char *) &arg, 0, sizeof(IonCXref));

	/*	Perform this interior loop until either the best route
	 *	to the end vertex has been identified or else it is
	 *	known that there is no such route.			*/

	while (1)
	{
		/*	Consider all unvisited successors (i.e., next-
		 *	hop contacts) of the current contact, in each
		 *	case computing best-case arrival time for a
		 *	bundle transmitted during that contact.		*/

		arg.fromNode = current->toNode;
		TRACE(CgrConsiderRoot, current->fromNode, current->toNode);

		/*	First, compute and note/revise/discard the
		 *	best-case bundle arrival time for all contacts
		 *	that are topologically adjacent to the current
		 *	contact.					*/

		for (oK(sm_rbt_search(ionwm, ionvdb->contactIndex,
				rfx_order_contacts, &arg, &elt));
				elt; elt = sm_rbt_next(ionwm, elt))
		{
			contact = (IonCXref *) psp(ionwm,
					sm_rbt_data(ionwm, elt));

			/*	Note: contact->fromNode can't be less
			 *	than current->toNode: we started at
			 *	that node with sm_rbt_search.		*/

			if (contact->fromNode > current->toNode)
			{
				/*	No more relevant contacts.	*/

				break;
			}

			TRACE(CgrConsiderContact, contact->fromNode,
					contact->toNode);
			if (contact->toTime <= currentWork->arrivalTime)
			{
				TRACE(CgrIgnoreContact, CgrContactEndsEarly);

				/*	Can't be a next-hop contact:
				 *	transmission has stopped by
				 *	the time of arrival of data
				 *	during the current contact.	*/

				continue;
			}

			work = (CgrContactNote *) psp(ionwm,
					contact->routingObject);
			CHKERR(work);
			if (work->suppressed)
			{
				TRACE(CgrIgnoreContact, CgrSuppressed);
				continue;
			}

			if (work->visited)
			{
				TRACE(CgrIgnoreContact, CgrVisited);
				continue;
			}

			/*	Get OWLT between the nodes in contact,
			 *	from applicable range in range index.	*/

			if (getApplicableRange(contact, &owlt) < 0)
			{
				TRACE(CgrIgnoreContact, CgrNoRange);

				/*	Don't know the OWLT between
				 *	these BP nodes at this time,
				 *	so can't consider in CGR.	*/

				work->suppressed = 1;
				continue;
			}

			/*	Compute contact capacity as needed.	*/

			if (work->capacity == 0)
			{
				work->capacity = contact->xmitRate *
					(contact->toTime - contact->fromTime);
			}

			/*	Allow for possible additional latency
			 *	due to the movement of the receiving
			 *	node during the propagation of signal
			 *	from the sending node.			*/

			owltMargin = ((MAX_SPEED_MPH / 3600) * owlt) / 186282;
			owlt += owltMargin;

			/*	Compute cost of choosing this edge:
			 *	earliest bundle arrival time, given
			 *	that the bundle arrives at the sending
			 *	node in the course of the current
			 *	contact.				*/

			if (contact->fromTime < currentWork->arrivalTime)
			{
				transmitTime = currentWork->arrivalTime;
			}
			else
			{
				transmitTime = contact->fromTime;
			}

			arrivalTime = transmitTime + owlt;

			/*	Note that this arrival time is best
			 *	case.  It is based on the earliest
			 *	possible transmit time, which would
			 *	be applicable to a bundle transmitted
			 *	on this route immediately; any delay
			 *	in transmission due to queueing behind
			 *	other bundles would result in a later
			 *	transmit time and therefore a later
			 *	arrival time.				*/

			TRACE(CgrCost, (unsigned int)(transmitTime), owlt,
					(unsigned int)(arrivalTime));

			if (arrivalTime < work->arrivalTime)
			{
				/*	Bundle arrival time will
				 *	vary with bundle transmit
				 *	time, which may be delayed
				 *	awaiting bundle arrival via
				 *	the current contact.		*/

				work->arrivalTime = arrivalTime;
				work->predecessor = current;
			}
		}

		/*	Have at this point computed the best-case
		 *	bundle arrival times for all edges of the
		 *	graph that originate at the current contact.	*/

		currentWork->visited = 1;

		/*	Now the second loop: among ALL non-suppressed
		 *	contacts in the graph, select the one with
		 *	the earliest arrival time (least distance
		 *	from the root vertex) as the new "current"
		 *	vertex to analyze.				*/

		nextContact = NULL;
		earliestArrivalTime = MAX_TIME;
		largestCapacity = 0;
		earliestStartTime = MAX_TIME;
		for (elt = sm_rbt_first(ionwm, ionvdb->contactIndex); elt;
				elt = sm_rbt_next(ionwm, elt))
		{
			contact = (IonCXref *) psp(ionwm, sm_rbt_data(ionwm,
					elt));
			CHKERR(contact);
			work = (CgrContactNote *) psp(ionwm,
					contact->routingObject);
			CHKERR(work);
			if (work->suppressed || work->visited)
			{
				continue;	/*	Ineligible.	*/
			}

			if (work->arrivalTime == MAX_TIME)
			{
				/*	Not reachable from root.	*/

				continue;
			}

			/*	Dijkstra search edge cost function.	*/

			if (work->arrivalTime > earliestArrivalTime)
			{
				/*	Not the lowest-cost edge.	*/

				continue;
			}

			if (work->arrivalTime < earliestArrivalTime)
			{
				/*	Best successor contact seen
				 *	so far.				*/

				nextContact = contact;
				earliestArrivalTime = work->arrivalTime;
				largestCapacity = work->capacity;
				earliestStartTime = contact->fromTime;
				continue;
			}

			/*	Same arrival time as current best
			 *	candidate next contact.			*/

			if (work->capacity < largestCapacity)
			{
				/*	Not the lowest-cost edge.	*/

				continue;
			}

			if (work->capacity > largestCapacity)
			{
				/*	Best successor contact seen
				 *	so far.				*/

				nextContact = contact;
				earliestArrivalTime = work->arrivalTime;
				largestCapacity = work->capacity;
				earliestStartTime = contact->fromTime;
				continue;
			}

			/*	Same arrival time and capacity as
			 *	current best candidate next contact.	*/

			if (contact->fromTime >= earliestStartTime)
			{
				/*	Not the lowest-cost edge.	*/

				continue;
			}

			/*	Best successor contact seen so far.	*/

			nextContact = contact;
			earliestArrivalTime = work->arrivalTime;
			largestCapacity = work->capacity;
			earliestStartTime = contact->fromTime;
		}

		/*	If search is complete, stop.  Else repeat,
		 *	with new value of "current".			*/

		if (nextContact == NULL)
		{
			/*	End of search; can't proceed any
			 *	further toward the terminal contact.	*/

			break;
		}

		current = nextContact;
		currentWork = (CgrContactNote *)
				psp(ionwm, nextContact->routingObject);
		if (current->toNode == terminusNode->nodeNbr)
		{
			earliestFinalArrivalTime = currentWork->arrivalTime;
			finalContact = current;
			break;
		}
	}

	/*	Have finished a single Dijkstra search of the contact
	 *	graph, excluding those contacts that were suppressed.	*/

	if (finalContact)	/*	Got route to terminal contact.	*/
	{
		route->arrivalTime = earliestFinalArrivalTime;
		route->arrivalConfidence = 1.0;

		/*	Load the entire route into the "hops" list,
		 *	backtracking to root, and compute the time
		 *	at which the route will become unusable.	*/

		earliestEndTime = MAX_TIME;
		maxCapacity = (uvast) -1;
		for (contact = finalContact; contact != rootContact;
				contact = work->predecessor)
		{
			if (contact->toTime < earliestEndTime)
			{
				earliestEndTime = contact->toTime;
			}

			work = (CgrContactNote *) psp(ionwm,
					contact->routingObject);
			if (work->capacity < maxCapacity)
			{
				maxCapacity = work->capacity;
			}

			route->arrivalConfidence *= contact->confidence;
			addr = psa(ionwm, contact);
			TRACE(CgrHop, contact->fromNode, contact->toNode);
			if (sm_list_insert_first(ionwm, route->hops, addr) == 0)
			{
				putErrmsg("Can't insert contact into route.",
						NULL);
				return -1;
			}
		}

		/*	Now use the first contact in the route to
		 *	characterize the route.				*/

		addr = sm_list_data(ionwm, sm_list_first(ionwm, route->hops));
		contact = (IonCXref *) psp(ionwm, addr);
		route->toNodeNbr = contact->toNode;
		route->fromTime = contact->fromTime;
		route->toTime = earliestEndTime;
		route->maxCapacity = maxCapacity;
	}

	return 0;
}

static int	findNextBestRoute(PsmPartition ionwm, IonCXref *rootContact,
			CgrContactNote *rootWork, IonNode *terminusNode,
			PsmAddress *routeAddr, CgrTrace *trace)
{
	PsmAddress	addr;
	CgrRoute	*route;

	*routeAddr = 0;		/*	Default.			*/
	addr = psm_zalloc(ionwm, sizeof(CgrRoute));
	if (addr == 0)
	{
		putErrmsg("Can't create CGR route.", NULL);
		return -1;
	}

	route = (CgrRoute *) psp(ionwm, addr);
	memset((char *) route, 0, sizeof(CgrRoute));
	route->hops = sm_list_create(ionwm);
	if (route->hops == 0)
	{
		psm_free(ionwm, addr);
		putErrmsg("Can't create CGR route hops list.", NULL);
		return -1;
	}

	/*	Run Dijkstra search.					*/

	if (computeDistanceToTerminus(rootContact, rootWork, terminusNode,
			route, trace) < 0)
	{
		putErrmsg("Can't finish Dijstra search.", NULL);
		return -1;
	}

	if (route->toNodeNbr == 0)
	{
		TRACE(CgrDiscardRoute);

		/*	No more routes found in graph.			*/

		sm_list_destroy(ionwm, route->hops, NULL, NULL);
		psm_free(ionwm, addr);
		*routeAddr = 0;
	}
	else
	{
		TRACE(CgrAcceptRoute, route->toNodeNbr,
				(unsigned int)(route->fromTime),
				(unsigned int)(route->arrivalTime),
				route->maxCapacity);

		/*	Found best route, given current exclusions.	*/

		*routeAddr = addr;
	}

	return 0;
}

static PsmAddress	loadRouteList(IonNode *terminusNode, time_t currentTime,
				CgrTrace *trace)
{
	PsmPartition	ionwm = getIonwm();
	IonVdb		*ionvdb = getIonVdb();
	CgrVdb		*cgrvdb = getCgrVdb();
	PsmAddress	elt;
	IonCXref	*contact;
	CgrContactNote	*work;
	IonCXref	rootContact;
	CgrContactNote	rootWork;
	PsmAddress	routeAddr;
	CgrRoute	*route;
	IonCXref	*firstContact;

	CHKZERO(ionvdb);
	CHKZERO(cgrvdb);

	/*	Loading the list of routes from the local node to
	 *	a given terminus node is a process involving three
	 *	layers of loops.
	 *
	 *	The outermost "exterior" loop is performed once per
	 *	neighbor of the local node, to find the best route
	 *	(identified by initial contact) from the local node
	 *	to the terminus through that neighbor.
	 *
	 *	Each execution of the exterior loop performs a
	 *	Dijkstra search (an inner "interior" loop as
	 *	described below) that finds the best route through
	 *	all contacts that have not yet been suppressed,
	 *	followed by a loop that flags as "suppressed" all
	 *	contacts from the local node to the neighbor that
	 *	is the entry node for that newly discovered route.
	 *
	 *	The interior loop is performed repeatedly, starting 
	 *	with the "current" vertex being set to the root
	 *	of the contact graph (an artificial pseudo-contact
	 *	from the local node to itself).  Each execution of
	 *	the interior loop performs two loops in succession.
	 *
	 *	The first of these innermost loops is a scan of
	 *	all contacts that are topologically adjacent to the
	 *	"current" contact.  For each such contact, the
	 *	"distance" of that vertex from the root of the
	 *	graph (the best-case arrival time of a bundle at
	 *	the receiving node of this contact, assuming
	 *	tranmission from the current contact) is computed;
	 *	if that time is earlier than the previously noted
	 *	best-case arrival time for that contact, then it
	 *	is noted as the new best-case arrival time for
	 *	that contact.
	 *
	 *	The second of these innermost loops through all
	 *	(non-suppressed) contacts, seeking the contact that
	 *	now has the earliest best-case arrival time (the
	 *	vertex with lowest cost) in the entire graph, and
	 *	selects that vertext as the new "current" contact.
	 *
	 *	The interior loop terminates when either no new
	 *	current contact can be selected or else the new
	 *	current contact is the end vertex of the graph
	 *	(an artificial pseudo-contact from the terminus
	 *	node to itself).					*/

	/*	First create route list for this destination node.	*/

	terminusNode->routingObject = sm_list_create(ionwm);
	if (terminusNode->routingObject == 0)
	{
		putErrmsg("Can't create CGR route list.", NULL);
		return 0;
	}

	oK(sm_list_user_data_set(ionwm, terminusNode->routingObject,
			psa(ionwm, terminusNode)));
	if (sm_list_insert_last(ionwm, cgrvdb->routeLists,
			terminusNode->routingObject) == 0)
	{
		putErrmsg("Can't note CGR route list.", NULL);
		return 0;
	}

	/*	Now discover the best routes (transmission sequences,
	 *	paths, itineraries) from the local node that can
	 *	result in arrival at the remote node.  To do this, we
	 *	run a series of Dijkstra searches through the contact
	 *	graph, rooted at a dummy contact from the local node
	 *	to itself and terminating in the "final contact"
	 *	(which is the terminus node's contact with itself).
	 *	Each time we search, we exclude from consideration
	 *	all contacts with neighboring nodes through which
	 *	optimal contacts have already been discovered.		*/

	rootContact.fromNode = getOwnNodeNbr();
	rootContact.toNode = rootContact.fromNode;
	rootWork.arrivalTime = currentTime;

	/*	Clear all contact work areas.				*/

	for (elt = sm_rbt_first(ionwm, ionvdb->contactIndex); elt;
			elt = sm_rbt_next(ionwm, elt))
	{
		contact = (IonCXref *) psp(ionwm, sm_rbt_data(ionwm, elt));
		if ((work = (CgrContactNote *) psp(ionwm,
				contact->routingObject)) == 0)
		{
			contact->routingObject = psm_zalloc(ionwm,
					sizeof(CgrContactNote));
			work = (CgrContactNote *) psp(ionwm,
					contact->routingObject);
			if (work == 0)
			{
				putErrmsg("Can't create contact note.", NULL);
				return 0;
			}
		}

		memset((char *) work, 0, sizeof(CgrContactNote));
		work->arrivalTime = MAX_TIME;
	}

	/*	Find best routes through this contact graph via each
	 *	of the nodes that are neighbors of the local node.	*/

	while (1)
	{
		/*	Run Dijkstra search.				*/

		if (findNextBestRoute(ionwm, &rootContact, &rootWork,
				terminusNode, &routeAddr, trace) < 0)
		{
			putErrmsg("Can't load routes list.", NULL);
			return 0;
		}

		if (routeAddr == 0)
		{
			break;		/*	No more routes.		*/
		}

		/*	Found optimal route, given exclusion of all
		 *	contacts with neighboring nodes through which
		 *	optimal contacts have already been discovered.	*/

		if (sm_list_insert_last(ionwm, terminusNode->routingObject,
				routeAddr) == 0)
		{
			putErrmsg("Can't add route to list.", NULL);
			return 0;
		}

		/*	Now exclude all contacts for transmission to
		 *	that neighboring node to which we transmit in
		 *	the course of the first contact in this
		 *	optimal route.  That is, we have determined
		 *	the best route through this neighboring node,
		 *	so now we are no longer interested in routes
		 *	that go through that node; we want the best
		 *	route through any of the neighboring nodes
		 *	for which we have not yet determined the
		 *	best route.  Clear the work areas for all
		 *	other contacts.					*/

		route = (CgrRoute *) psp(ionwm, routeAddr);
		firstContact = (IonCXref *) psp(ionwm, sm_list_data(ionwm,
				sm_list_first(ionwm, route->hops)));
		for (elt = sm_rbt_first(ionwm, ionvdb->contactIndex); elt;
				elt = sm_rbt_next(ionwm, elt))
		{
			contact = (IonCXref *) psp(ionwm, sm_rbt_data(ionwm,
					elt));
			work = (CgrContactNote *) psp(ionwm,
					contact->routingObject);
			if (contact->toNode == firstContact->toNode)
			{
				work->suppressed = 1;
			}

			if (work->suppressed)
			{
				continue;
			}

			/*	Contact may still be useful, so clear
			 *	its work area.				*/

			work->arrivalTime = MAX_TIME;
			work->predecessor = NULL;
			work->visited = 0;
		}

		/*	Now look for next route.			*/
	}

	return terminusNode->routingObject;
}

/*	Functions for identifying viable proximate nodes for forward
 *	transmission of a given bundle.					*/

static int	recomputeRouteThroughNeighbor(uvast neighborNodeNbr,
			IonNode *terminusNode, time_t currentTime,
			CgrTrace *trace)
{
	PsmPartition	ionwm = getIonwm();
	IonVdb		*vdb = getIonVdb();
	PsmAddress	routes;
	uvast		localNodeNbr = getOwnNodeNbr();
	PsmAddress	elt;
	IonCXref	*contact;
	CgrContactNote	*work;
	IonCXref	rootContact;
	CgrContactNote	rootWork;
	PsmAddress	routeAddr;

	TRACE(CgrRecomputeRoute);
	routes = terminusNode->routingObject;

	/*	Suppress from consideration every contact that
	 *	constitutes transmission from the local node to
	 *	any node other than the subject neighbor.  Clear
	 *	the work areas for all other contacts.			*/

	for (elt = sm_rbt_first(ionwm, vdb->contactIndex); elt;
			elt = sm_rbt_next(ionwm, elt))
	{
		contact = (IonCXref *) psp(ionwm, sm_rbt_data(ionwm, elt));
		if ((work = (CgrContactNote *) psp(ionwm,
				contact->routingObject)) == 0)
		{
			contact->routingObject = psm_zalloc(ionwm,
					sizeof(CgrContactNote));
			work = (CgrContactNote *) psp(ionwm,
					contact->routingObject);
			if (work == 0)
			{
				putErrmsg("Can't create CGR contact note.",
						NULL);
				return -1;
			}
		}

		if (contact->fromNode == localNodeNbr
		&& contact->toNode != neighborNodeNbr)
		{
			work->suppressed = 1;
		}
		else
		{
			memset((char *) work, 0, sizeof(CgrContactNote));
			work->arrivalTime = MAX_TIME;
		}
	}

	/*	Next invoke findNextBestRoute to discover the best
	 *	route through this neighbor given the revised cotact
	 *	graph.							*/

	rootContact.toNode = rootContact.fromNode = localNodeNbr;
	rootWork.arrivalTime = currentTime;
	if (findNextBestRoute(ionwm, &rootContact, &rootWork, terminusNode,
			&routeAddr, trace) < 0)
	{
		putErrmsg("Can't recompute route.", NULL);
		return -1;
	}

	if (routeAddr == 0)		/*	No route computed.	*/
	{
		return 0;
	}

	/*	Finally, insert that route into the terminusNode's
	 *	list of routes.						*/

	if (sm_list_insert_last(ionwm, routes, routeAddr) == 0)
	{
		putErrmsg("Can't insert recomputed route.", NULL);
		return -1;
	}

	return 1;
}

static int	isExcluded(uvast nodeNbr, Lyst excludedNodes)
{
	LystElt	elt;
	NodeId	*node;

	for (elt = lyst_first(excludedNodes); elt; elt = lyst_next(elt))
	{
		node = (NodeId *) lyst_data(elt);
		if (node->nbr == nodeNbr)
		{
			return 1;	/*	Node is in the list.	*/
		}
	}

	return 0;
}

static time_t	computeArrivalTime(CgrRoute *route, Bundle *bundle,
			time_t currentTime, BpPlan *plan, Scalar *overbooked,
			Scalar *protected, time_t *eto)
{
	PsmPartition	ionwm = getIonwm();
	IonVdb		*vdb = getIonVdb();
	uvast		ownNodeNbr = getOwnNodeNbr();
	Scalar		priorClaims;
	Scalar		totalBacklog;
	IonCXref	arg;
	PsmAddress	elt;
	IonCXref	*contact;
	Scalar		capacity;
	Scalar		allotment;
	int		eccc;	/*	Estimated capacity consumption.	*/
	time_t		startTime;
	time_t		endTime;
	int		secRemaining;
	time_t		transmitTime;
	Scalar		radiationLatency;
	unsigned int	owlt;
	unsigned int	owltMargin;
	time_t		arrivalTime;

	computePriorClaims(plan, bundle, &priorClaims, &totalBacklog);
	copyScalar(protected, &totalBacklog);

	/*	Reduce prior claims on the first contact in this
	 *	route by all transmission to this contact's neighbor
	 *	that will be performed during contacts that precede
	 *	this contact.						*/

	loadScalar(&allotment, 0);
	loadScalar(&capacity, 0);
	memset((char *) &arg, 0, sizeof(IonCXref));
	arg.fromNode = ownNodeNbr;
	arg.toNode = route->toNodeNbr;
	for (oK(sm_rbt_search(ionwm, vdb->contactIndex, rfx_order_contacts,
			&arg, &elt)); elt; elt = sm_rbt_next(ionwm, elt))
	{
		contact = (IonCXref *) psp(ionwm, sm_rbt_data(ionwm, elt));
		if (contact->fromNode > ownNodeNbr
		|| contact->toNode > route->toNodeNbr
		|| contact->fromTime > route->fromTime)
		{
			/*	Initial contact on route has expired
			 *	and has been removed (but the route
			 *	itself has not yet been removed per
			 *	the identifyProximateNodes procedure).	*/

			return 0;
		}

		if (contact->toTime < currentTime)
		{
			/*	This contact has already terminated.	*/

			continue;
		}

		/*	Compute capacity of contact.			*/

		if (currentTime > contact->fromTime)
		{
			startTime = currentTime;
		}
		else
		{
			startTime = contact->fromTime;
		}

		endTime = contact->toTime;
		secRemaining = endTime - startTime;
		loadScalar(&capacity, secRemaining);
		multiplyScalar(&capacity, contact->xmitRate);

		/*	Determine how much spare capacity the
		 *	contact has.					*/

		copyScalar(&allotment, &capacity);
		subtractFromScalar(&allotment, protected);
		if (!scalarIsValid(&allotment))
		{
			/*	Capacity is less than remaining
			 *	backlog, so the contact is fully
			 *	subscribed.				*/

			copyScalar(&allotment, &capacity);
		}
		else
		{
			/*	Capacity is greater than or equal to
			 *	the remaining backlog, so the last of
			 *	the backlog will be served by this
			 *	contact, possibly with some capacity
			 *	left over.				*/

			copyScalar(&allotment, protected);
		}

		/*	Determine how much of the total backlog has
		 *	been allotted to subsequent contacts.		*/

		subtractFromScalar(protected, &capacity);
		if (!scalarIsValid(protected))
		{
			/*	No bundles scheduled for transmission
			 *	during any subsequent contacts.		*/

			loadScalar(protected, 0);
		}

		/*	Loop limit check.				*/

		if (contact->fromTime >= route->fromTime)
		{
			/*	This is the initial contact on the
			 *	route we are considering.  All prior
			 *	contacts have been allocated to prior
			 *	transmission claims.			*/

			break;
		}

		/*	This is a contact that precedes the initial
		 *	contact on the route we are considering.
		 *	Determine how much of the prior claims on
		 *	the route's first contact will be served by
		 *	this contact.					*/

		subtractFromScalar(&priorClaims, &capacity);
		if (!scalarIsValid(&priorClaims))
		{
			/*	Last of the prior claims will be
			 *	served by this contact.			*/

			loadScalar(&priorClaims, 0);
		}
	}

	/*	At this point, priorClaims contains the applicable
	 *	"residual backlog."
	 *
	 *	Now consider the initial contact on the route.
	 *	First, check for potential overbooking.			*/

	eccc = computeECCC(guessBundleSize(bundle));
	copyScalar(overbooked, &allotment);
	increaseScalar(overbooked, eccc);
	subtractFromScalar(overbooked, &capacity);
	if (!scalarIsValid(overbooked))
	{
		loadScalar(overbooked, 0);
	}

	/*	Now compute expected initial transmit time 
	 *	(Earliest Transmission Opportunity): start of
	 *	initial contact plus delay imposed by transmitting
	 *	all remaining prior claims plus this bundle itself,
	 *	at the transmission rate of the initial contact.
	 *	Here ETO indicates the time at which transmission
	 *	has been COMPLETED (not started) so that arrival
	 *	time can likewise indicate the time at which arrival
	 *	has completed (not started).				*/

	if (currentTime > route->fromTime)
	{
		transmitTime = currentTime;
	}
	else
	{
		transmitTime = route->fromTime;
	}

	copyScalar(&radiationLatency, &priorClaims);
	increaseScalar(&radiationLatency, eccc);
	elt = sm_list_first(ionwm, route->hops);
	contact = (IonCXref *) psp(ionwm, sm_list_data(ionwm, elt));
	CHKERR(contact->xmitRate > 0);
	divideScalar(&radiationLatency, contact->xmitRate);
	transmitTime += ((ONE_GIG * radiationLatency.gigs)
			+ radiationLatency.units);
	*eto = transmitTime;

	/*	Note that eto now contains the "last byte
	 *	transmission time" for transmitting this bundle
	 *	during the initial contact of this route.
	 *
	 *	Now compute expected final arrival time by adding
	 *	OWLTs, inter-contact delays, and per-hop radiation
	 *	latencies along the path to the terminus node.		*/

	while (1)
	{
		if (transmitTime >= contact->toTime)
		{
			/*	Due to the volume of transmission
			 *	that must precede it, this bundle
			 *	can't be fully transmitted during this
			 *	contact.  So the route is unusable.
			 *
			 *	NOTE that the SABR concept of
			 *	anticipatory fragmentation is NOT
			 *	implemented in ION at this time.
			 *
			 *	Note that transmit time is computed
			 *	using integer arithmetic, which will
			 *	truncate any fractional seconds of
			 *	total transmission time.  To account
			 *	for this rounding error, we require
			 *	that the computed transmit time be
			 *	less than the contact end time,
			 *	rather than merely not greater.		*/

			return 0;
		}

		if (getApplicableRange(contact, &owlt) < 0)
		{
			/*	Can't determine owlt for this contact,
			 *	so arrival time can't be computed.
			 *	Route is not usable.			*/

			return 0;
		}

		owltMargin = ((MAX_SPEED_MPH / 3600) * owlt) / 186282;
		arrivalTime = transmitTime + owlt + owltMargin;

		/*	Now check next contact in the end-to-end path.	*/

		elt = sm_list_next(ionwm, elt);
		if (elt == 0)
		{
			break;	/*	End of route.			*/
		}

		/*	Not end of route, so the "to" node for this
		 *	contact is not the terminus node, i.e., the
		 *	bundle must be forwarded from this node.	*/

		contact = (IonCXref *) psp(ionwm, sm_list_data(ionwm, elt));
		if (arrivalTime > contact->fromTime)
		{
			transmitTime = arrivalTime;
		}
		else
		{
			transmitTime = contact->fromTime;
		}

		/*	Consider additional latency imposed by the
		 *	time required to transmit all bytes of the
		 *	bundle.  At each hop of the path, additional
		 *	radiation latency is computed as bundle size
		 *	divided by data rate.				*/

		loadScalar(&radiationLatency, eccc);
		divideScalar(&radiationLatency, contact->xmitRate);
		transmitTime += ((ONE_GIG * radiationLatency.gigs)
				+ radiationLatency.units);
	}

	if (arrivalTime > (bundle->expirationTime + EPOCH_2000_SEC))
	{
		/*	Bundle will never arrive: it will expire
		 *	before arrival.					*/

		arrivalTime = 0;
	}

	return arrivalTime;
}

static int	tryRoute(CgrRoute *route, time_t currentTime, Bundle *bundle,
			CgrTrace *trace, Lyst proximateNodes)
{
	Sdr		sdr = getIonsdr();
	PsmPartition	ionwm = getIonwm();
	char		eid[SDRSTRING_BUFSZ];
	VPlan		*vplan;
	PsmAddress	vplanElt;
	Object		planObj;
	BpPlan		plan;
	int		hopCount;
	uvast		capacity;
	time_t		arrivalTime;
	Scalar		overbooked;
	Scalar		protected;
	time_t		eto;
	ProximateNode	*proxNode;

	isprintf(eid, sizeof eid, "ipn:" UVAST_FIELDSPEC ".0",
			route->toNodeNbr);
	findPlan(eid, &vplan, &vplanElt);
	if (vplanElt == 0)
	{
		TRACE(CgrIgnoreRoute, CgrNoPlan);
		return 0;		/*	No egress plan to node.	*/
	}

	planObj = sdr_list_data(sdr, vplan->planElt);
	sdr_read(sdr, (char *) &plan, planObj, sizeof(BpPlan));

	/*	Now determine whether or not the bundle could be sent
	 *	to this neighbor via the applicable egress plan in
	 *	time to follow the route that is being considered
	 *	(i.e., the best route through this neighbor).  There
	 *	are two criteria.  First, is the egress plan blocked
	 *	(i.e., temporarily shut off by operations)?		*/

	if (plan.blocked)
	{
		TRACE(CgrIgnoreRoute, CgrBlockedPlan);
		return 0;		/*	Node is unreachable.	*/
	}

	/*	Second: if this bundle were sent on this route, given
	 *	all other bundles enqueued ahead of it, could it make
	 *	all of its contact connections in time to arrive
	 *	before its expiration time?  For this purpose we need
	 *	to scan the scheduled intervals of contact with the
	 *	candidate neighbor.					*/

	arrivalTime = computeArrivalTime(route, bundle, currentTime,
			&plan, &overbooked, &protected, &eto);
	if (arrivalTime == 0)	/*	Can't be delivered in time.	*/
	{
		TRACE(CgrIgnoreRoute, CgrRouteTooSlow);
		return 0;		/*	Connections too tight.	*/
	}

	/*	This route is a plausible opportunity for getting
	 *	the bundle forwarded to the terminus node before it
	 *	expires, so we add the route's entry node to the
	 *	list of candidate proximate nodes for this bundle.
	 *
	 *	The arrivalTime noted for a proximate node is the
	 *	projected arrival time for the best route that
	 *	starts with transmission to that neighboring node.
	 *
	 *	The capacity noted here is the capacity of the
	 *	best route that starts with transmission to the
	 *	proximate node.
	 *
	 *	The hopCount noted here is the hop count for the
	 *	best route that starts with transmission to the
	 *	proximate node.
	 *
	 *	We set forfeit time to the forfeit time associated
	 *	with the best route that starts with transmission
	 *	to the proximate node.					*/

	capacity = route->maxCapacity;
	hopCount = sm_list_length(ionwm, route->hops);
	proxNode = (ProximateNode *) MTAKE(sizeof(ProximateNode));
	if (proxNode == NULL
	|| lyst_insert_last(proximateNodes, (void *) proxNode) == 0)
	{
		putErrmsg("Can't add proximateNode.", NULL);
		return -1;
	}

	proxNode->neighborNodeNbr = route->toNodeNbr;
	proxNode->arrivalTime = arrivalTime;
	proxNode->capacity = capacity;
	proxNode->hopCount = hopCount;
	proxNode->forfeitTime = route->toTime;
	copyScalar(&proxNode->overbooked, &overbooked);
	copyScalar(&proxNode->protected, &protected);
	proxNode->arrivalConfidence = route->arrivalConfidence;
	TRACE(CgrAddProximateNode);
	return 0;
}

static int	identifyProximateNodes(IonNode *terminusNode, Bundle *bundle,
			Object bundleObj, Lyst excludedNodes, CgrTrace *trace,
			Lyst proximateNodes, time_t currentTime)
{
	PsmPartition	ionwm = getIonwm();
	unsigned int	deadline;
	PsmAddress	routes;		/*	SmList of CgrRoutes.	*/
	PsmAddress	elt;
	PsmAddress	nextElt;
	PsmAddress	addr;
	IonCXref	*contact;
	CgrRoute	*route;
	uvast		contactToNodeNbr;

	deadline = bundle->expirationTime + EPOCH_2000_SEC;

	/*	Examine all opportunities for transmission to any
	 *	neighboring node that would result in arrival at
	 *	the terminus node.					*/

	routes = terminusNode->routingObject;
	if (routes == 0)	/*	No current routes to this node.	*/
	{
		if ((routes = loadRouteList(terminusNode, currentTime, trace))
				== 0)
		{
			putErrmsg("Can't load routes for node.",
					utoa(terminusNode->nodeNbr));
			return -1;
		}
	}

	TRACE(CgrIdentifyProximateNodes, deadline);
	for (elt = sm_list_first(ionwm, routes); elt; elt = nextElt)
	{
		nextElt = sm_list_next(ionwm, elt);
		addr = sm_list_data(ionwm, elt);
		route = (CgrRoute *) psp(ionwm, addr);
		TRACE(CgrCheckRoute, route->toNodeNbr,
				(unsigned int)(route->fromTime),
				(unsigned int)(route->arrivalTime));
		if (route->toTime < currentTime)
		{
			/*	This route includes a contact that
			 *	has already ended; delete it.		*/

			contactToNodeNbr = route->toNodeNbr;
			if (route->hops)
			{
				sm_list_destroy(ionwm, route->hops, NULL, NULL);
			}

			psm_free(ionwm, addr);
			sm_list_delete(ionwm, elt, NULL, NULL);

			/*	Now compute the best remaining route
			 *	through this neighboring node.		*/

			switch (recomputeRouteThroughNeighbor(contactToNodeNbr,
					terminusNode, currentTime, trace))
			{
			case -1:
				putErrmsg("Route recomputation failed.", NULL);
				return -1;

			case 0:
				/*	No more routes through this
				 *	neighbor at this time.		*/

				break;

			default:
				/*	Best remaining route through
				 *	this neighboring node has been
				 *	computed and inserted into the
				 *	list of routes.  Must start
				 *	again from the beginning of
				 *	the list.			*/

				nextElt = sm_list_first(ionwm, routes);
			}

			continue;
		}

		if (route->arrivalTime > deadline)
		{
			/*	Not a plausible route.			*/

			continue;
		}

		addr = sm_list_data(ionwm, sm_list_first(ionwm, route->hops));
		contact = (IonCXref *) psp(ionwm, addr);
		if (contact->confidence != 1.0)
		{
			continue;	/*	Not currently usable.	*/
		}

		/*	Never route to self unless self is the final
		 *	destination.					*/

		if (route->toNodeNbr == getOwnNodeNbr())
		{
			if (!(bundle->destination.cbhe
			&& bundle->destination.c.nodeNbr == route->toNodeNbr))
			{
				/*	Never route via self -- a loop.	*/

				TRACE(CgrIgnoreRoute, CgrRouteViaSelf);
				continue;
			}

			/*	Self is final destination.		*/
		}

		/*	Is the bundle's size greater that the
		 *	capacity of whichever contact in this route
		 *	has the least capacity?  If so, can't use
		 *	this route.   (NOTE: the SABR concept of
		 *	anticipatory fragmentation is not implemented
		 *	in ION at this time.)				*/

		if (bundle->payload.length > route->maxCapacity)
		{
			TRACE(CgrIgnoreRoute, CgrRouteCapacityTooSmall);
			continue;
		}

		/*	Is the neighbor that receives bundles during
		 *	this route's initial contact excluded for any
		 *	reason?						*/

		if (isExcluded(route->toNodeNbr, excludedNodes))
		{
			TRACE(CgrIgnoreRoute, CgrInitialContactExcluded);
			continue;
		}

		/*	Route might work.  If this route is supported
		 *	by contacts with enough aggregate capacity to
		 *	convey this bundle and all currently queued
		 *	bundles of equal or higher priority, then the
		 *	neighbor is a candidate proximate node for
		 *	forwarding the bundle to the terminus node.	*/

		if (tryRoute(route, currentTime, bundle, trace, proximateNodes)
				< 0)
		{
			putErrmsg("Can't check route.", NULL);
			return -1;
		}
	}

	return 0;
}

/*		Functions for forwarding bundle to selected neighbor.	*/

static void	deleteObject(LystElt elt, void *userdata)
{
	void	*object = lyst_data(elt);

	if (object)
	{
		MRELEASE(lyst_data(elt));
	}
}

static int	excludeNode(Lyst excludedNodes, uvast nodeNbr)
{
	NodeId	*node = (NodeId *) MTAKE(sizeof(NodeId));

	if (node == NULL)
	{
		return -1;
	}

	node->nbr = nodeNbr;
	if (lyst_insert_last(excludedNodes, node) == NULL)
	{
		return -1;
	}

	return 0;
}

static float	getNewDlvConfidence(Bundle *bundle, ProximateNode *proxNode)
{
	float		dlvFailureConfidence;

	/*	Delivery of bundle fails if and only if all forwarded
	 *	copies fail to arrive.  Our confidence that this will
	 *	happen is the product of our confidence in the delivery
	 *	failures of all forwarded copies, each of which is
	 *	1.0 minus our confidence that this copy will arrive.	*/

	dlvFailureConfidence = (1.0 - bundle->dlvConfidence)
			* (1.0 - proxNode->arrivalConfidence);
	return (1.0 - dlvFailureConfidence);
}

static int	enqueueToNeighbor(ProximateNode *proxNode, Bundle *bundle,
			Object bundleObj, IonNode *terminusNode)
{
	unsigned int	serviceNbr;
	char		terminusEid[64];
	PsmPartition	ionwm;
	PsmAddress	embElt;
	Embargo		*embargo;
	BpEvent		event;
	char		neighborEid[MAX_EID_LEN + 1];
	VPlan		*vplan;
	PsmAddress	vplanElt;

	/*	Note that a copy is being sent on the route through
	 *	this neighbor.						*/

	if (bundle->xmitCopiesCount == MAX_XMIT_COPIES)
	{
		return 0;	/*	Reached forwarding limit.	*/
	}

	bundle->xmitCopies[bundle->xmitCopiesCount] = proxNode->neighborNodeNbr;
	bundle->xmitCopiesCount++;
	bundle->dlvConfidence = getNewDlvConfidence(bundle, proxNode);
	if (proxNode->neighborNodeNbr == bundle->destination.c.nodeNbr)
	{
		serviceNbr = bundle->destination.c.serviceNbr;
	}
	else
	{
		serviceNbr = 0;
	}

	isprintf(terminusEid, sizeof terminusEid, "ipn:" UVAST_FIELDSPEC ".%u",
			proxNode->neighborNodeNbr, serviceNbr);

	/*	If this neighbor is a currently embargoed neighbor
	 *	for this final destination (i.e., one that has been
	 *	refusing bundles destined for this final destination
	 *	node), then this bundle serves as a "probe" aimed at
	 *	that neighbor.  In that case, must now enable the
	 *	scheduling of the next probe to this neighbor.		*/

	ionwm = getIonwm();
	for (embElt = sm_list_first(ionwm, terminusNode->embargoes);
			embElt; embElt = sm_list_next(ionwm, embElt))
	{
		embargo = (Embargo *) psp(ionwm, sm_list_data(ionwm, embElt));
		if (embargo->nodeNbr < proxNode->neighborNodeNbr)
		{
			continue;
		}

		if (embargo->nodeNbr > proxNode->neighborNodeNbr)
		{
			break;
		}

		/*	This neighbor has been refusing bundles
		 *	destined for this final destination node,
		 *	but since it is now due for a probe bundle
		 *	(else it would have been on the excludedNodes
		 *	list and therefore would never have made it
		 *	to the list of proximateNodes), we are
		 *	sending this one to it.  So we must turn
		 *	off the flag indicating that a probe to this
		 *	node is due -- we're sending one now.		*/

		embargo->probeIsDue = 0;
		break;
	}

	/*	If the bundle is NOT critical, then we need to post
	 *	an xmitOverdue timeout event to trigger re-forwarding
	 *	in case the bundle doesn't get transmitted during the
	 *	contact in which we expect it to be transmitted.	*/

	if (!(bundle->ancillaryData.flags & BP_MINIMUM_LATENCY))
	{
		event.type = xmitOverdue;
		event.time = proxNode->forfeitTime;
		event.ref = bundleObj;
		bundle->overdueElt = insertBpTimelineEvent(&event);
		if (bundle->overdueElt == 0)
		{
			putErrmsg("Can't schedule xmitOverdue.", NULL);
			return -1;
		}

		sdr_write(getIonsdr(), bundleObj, (char *) bundle,
				sizeof(Bundle));
	}

	/*	In any event, we enqueue the bundle for transmission.
	 *	Since we've already determined that the plan to this
	 *	neighbor is not blocked (else the neighbor would not
	 *	be in the list of proximate nodes), the bundle can't
	 *	go into limbo at this point.				*/

	isprintf(neighborEid, sizeof neighborEid, "ipn:" UVAST_FIELDSPEC ".0",
			proxNode->neighborNodeNbr);
	findPlan(neighborEid, &vplan, &vplanElt);
	CHKERR(vplanElt);
	if (bpEnqueue(vplan, bundle, bundleObj) < 0)
	{
		putErrmsg("Can't enqueue bundle.", NULL);
		return -1;
	}

	return 0;
}

#if (MANAGE_OVERBOOKING == 1)
typedef struct
{
	Object	currentElt;	/*	SDR list element.		*/
	Object	limitElt;	/*	SDR list element.		*/
} QueueControl;

static Object	getUrgentLimitElt(BpPlan *plan, int ordinal)
{
	Sdr	sdr = getIonsdr();
	int	i;
	Object	limitElt;

	/*	Find last bundle enqueued for the lowest ordinal
	 *	value that is higher than the bundle's ordinal;
	 *	limit elt is the next bundle in the urgent queue
	 *	following that one (i.e., the first enqueued for
	 *	the bundle's ordinal).  If none, then the first
	 *	bundle in the urgent queue is the limit elt.		*/

	for (i = ordinal + 1; i < 256; i++)
	{
		limitElt = plan->ordinals[i].lastForOrdinal;
		if (limitElt)
		{
			return sdr_list_next(sdr, limitElt);
		}
	}

	return sdr_list_first(sdr, plan->urgentQueue);
}

static Object	nextBundle(QueueControl *queueControls, int *queueIdx)
{
	Sdr		sdr = getIonsdr();
	QueueControl	*queue;
	Object		currentElt;

	queue = queueControls + *queueIdx;
	while (queue->currentElt == 0)
	{
		(*queueIdx)++;
		if ((*queueIdx) > BP_EXPEDITED_PRIORITY)
		{
			return 0;
		}

		queue++;
	}

	currentElt = queue->currentElt;
	if (currentElt == queue->limitElt)
	{
		queue->currentElt = 0;
	}
	else
	{
		queue->currentElt = sdr_list_prev(sdr, queue->currentElt);
	}

	return currentElt;
}

static int	manageOverbooking(ProximateNode *neighbor, Bundle *newBundle,
			CgrTrace *trace)
{
	Sdr		sdr = getIonsdr();
	char		neighborEid[MAX_EID_LEN + 1];
	VPlan		*vplan;
	PsmAddress	vplanElt;
	Object		planObj;
	BpPlan		plan;
	QueueControl	queueControls[] = { {0, 0}, {0, 0}, {0, 0} };
	int		queueIdx = 0;
	int		priority;
	int		ordinal;
	double		protected = 0.0;
	double		overbooked = 0.0;
	Object		elt;
	Object		bundleObj;
			OBJ_POINTER(Bundle, bundle);
	int		eccc;

	isprintf(neighborEid, sizeof neighborEid, "ipn:" UVAST_FIELDSPEC ".0",
			neighbor->neighborNodeNbr);
	priority = COS_FLAGS(newBundle->bundleProcFlags) & 0x03;
	if (priority == 0)
	{
		/*	New bundle's priority is Bulk, can't possibly
		 *	bump any other bundles.				*/

		return 0;
	}

	overbooked += (ONE_GIG * neighbor->overbooked.gigs)
			+ neighbor->overbooked.units;
	if (overbooked == 0.0)
	{
		return 0;	/*	No overbooking to manage.	*/
	}

	protected += (ONE_GIG * neighbor->protected.gigs)
			+ neighbor->protected.units;
	if (protected == 0.0)
	{
		TRACE(CgrPartialOverbooking, overbooked);
	}
	else
	{
		TRACE(CgrFullOverbooking, overbooked);
	}

	findPlan(neighborEid, &vplan, &vplanElt);
	if (vplanElt == 0)
	{
		TRACE(CgrIgnoreRoute, CgrNoPlan);

		return 0;		/*	No egress plan to node.	*/
	}

	planObj = sdr_list_data(sdr, vplan->planElt);
	sdr_read(sdr, (char *) &plan, planObj, sizeof(BpPlan));
	queueControls[0].currentElt = sdr_list_last(sdr, plan.bulkQueue);
	queueControls[0].limitElt = sdr_list_first(sdr, plan.bulkQueue);
	if (priority > 1)
	{
		queueControls[1].currentElt = sdr_list_last(sdr,
				plan.stdQueue);
		queueControls[1].limitElt = sdr_list_first(sdr,
				plan.stdQueue);
		ordinal = bundle->ancillaryData.ordinal;
		if (ordinal > 0)
		{
			queueControls[2].currentElt = sdr_list_last(sdr,
					plan.urgentQueue);
			queueControls[2].limitElt = getUrgentLimitElt(&plan,
					ordinal);
		}
	}

	while (overbooked > 0.0)
	{
		elt = nextBundle(queueControls, &queueIdx);
		if (elt == 0)
		{
			break;
		}

		bundleObj = sdr_list_data(sdr, elt);
		GET_OBJ_POINTER(sdr, Bundle, bundle, bundleObj);
		eccc = computeECCC(guessBundleSize(bundle));

		/*	Skip over all bundles that are protected
		 *	from overbooking because they are in contacts
		 *	following the contact in which the new bundle
		 *	is scheduled for transmission.			*/

		if (protected > 0.0)
		{
			protected -= eccc;
			continue;
		}

		/*	The new bundle has bumped this bundle out of
		 *	its originally scheduled contact.  Rebook it.	*/

		removeBundleFromQueue(bundle, bundleObj, planObj, &plan);
		if (bpReforwardBundle(bundleObj) < 0)
		{
			putErrmsg("Overbooking management failed.", NULL);
			return -1;
		}

		overbooked -= eccc;
	}

	return 0;
}
#endif

static int	proxNodeRedundant(Bundle *bundle, vast nodeNbr)
{
	int	i;

	for (i = 0; i < bundle->xmitCopiesCount; i++)
	{
		if (bundle->xmitCopies[i] == nodeNbr)
		{
			return 1;
		}
	}

	return 0;
}

static int	sendCriticalBundle(Bundle *bundle, Object bundleObj,
			IonNode *terminusNode, Lyst proximateNodes, int preview)
{
	LystElt		elt;
	LystElt		nextElt;
	ProximateNode	*proxNode;
	PsmAddress	routes;
	Bundle		newBundle;
	Object		newBundleObj;

	for (elt = lyst_first(proximateNodes); elt; elt = nextElt)
	{
		nextElt = lyst_next(elt);
		proxNode = (ProximateNode *) lyst_data_set(elt, NULL);
		lyst_delete(elt);
		if (preview)
		{
			MRELEASE(proxNode);
			continue;
		}

		if (proxNodeRedundant(bundle, proxNode->neighborNodeNbr))
		{
			MRELEASE(proxNode);
			continue;
		}

		if (bundle->planXmitElt)
		{
			/*	This copy of bundle has already
			 *	been enqueued.				*/

			if (bpClone(bundle, &newBundle, &newBundleObj,
					0, 0) < 0)
			{
				putErrmsg("Can't clone bundle.", NULL);
				lyst_destroy(proximateNodes);
				return -1;
			}

			bundle = &newBundle;
			bundleObj = newBundleObj;
		}

		if (enqueueToNeighbor(proxNode, bundle, bundleObj,
					terminusNode))
		{
			putErrmsg("Can't queue for neighbor.", NULL);
			lyst_destroy(proximateNodes);
			return -1;
		}

		MRELEASE(proxNode);
	}

	lyst_destroy(proximateNodes);
	if (bundle->dlvConfidence >= MIN_NET_DELIVERY_CONFIDENCE
	|| bundle->id.source.c.nodeNbr == bundle->destination.c.nodeNbr)
	{
		return 0;	/*	Potential future fwd unneeded.	*/
	}

	routes = terminusNode->routingObject;
	if (routes == 0 || sm_list_length(getIonwm(), routes) == 0)
	{
		return 0;	/*	No potential future forwarding.	*/
	}

	/*	Must put bundle in limbo, keep on trying to send it.	*/

	if (bundle->planXmitElt)
	{
		/*	This copy of bundle has already been enqueued.	*/

		if (bpClone(bundle, &newBundle, &newBundleObj, 0, 0) < 0)
		{
			putErrmsg("Can't clone bundle.", NULL);
			return -1;
		}

		bundle = &newBundle;
		bundleObj = newBundleObj;
	}

	if (enqueueToLimbo(bundle, bundleObj) < 0)
	{
		putErrmsg("Can't put bundle in limbo.", NULL);
		return -1;
	}

	return 0;
}

static int 	cgrForward(Bundle *bundle, Object bundleObj,
			uvast terminusNodeNbr, time_t atTime, CgrTrace *trace,
			int preview)
{
	IonVdb		*ionvdb = getIonVdb();
	CgrVdb		*cgrvdb = getCgrVdb();
	IonNode		*terminusNode;
	PsmAddress	nextNode;
	int		ionMemIdx;
	Lyst		proximateNodes;
	Lyst		excludedNodes;
	PsmPartition	ionwm = getIonwm();
	PsmAddress	embElt;
	Embargo		*embargo;
	LystElt		elt;
	LystElt		nextElt;
	ProximateNode	*proxNode;
	PsmAddress	routes;
	Bundle		newBundle;
	Object		newBundleObj;
	ProximateNode	*selectedNeighbor;
	float		newDlvConfidence;
	float		confidenceImprovement;

	/*	Determine whether or not the contact graph for the
	 *	terminus node identifies one or more proximate nodes
	 *	to which the bundle may be sent in order to get it
	 *	delivered to the terminus node.  If so, use the Plan
	 *	asserted for the best proximate node(s) ("dynamic
	 *	route").
	 *
	 *	Note that CGR can be used to compute a route to an
	 *	intermediate "station" node selected by another
	 *	routing mechanism (such as static routing), not
	 *	only to the bundle's final destination node.  In
	 *	the simplest case, the bundle's destination is the
	 *	only "station" selected for the bundle.  To avoid
	 *	confusion, we here use the term "terminus" to refer
	 *	to the node to which a route is being computed,
	 *	regardless of whether that node is the bundle's
	 *	final destination or an intermediate forwarding
	 *	station.			 			*/

	CHKERR(bundle && bundleObj && terminusNodeNbr);

	TRACE(CgrBuildRoutes, terminusNodeNbr, bundle->payload.length,
			(unsigned int)(atTime));

	if (ionvdb->lastEditTime.tv_sec > cgrvdb->lastLoadTime.tv_sec
	|| (ionvdb->lastEditTime.tv_sec == cgrvdb->lastLoadTime.tv_sec
	    && ionvdb->lastEditTime.tv_usec > cgrvdb->lastLoadTime.tv_usec)) 
	{
		/*	Contact plan has been modified, so must discard
		 *	all route lists and reconstruct them as needed.	*/

		discardRouteLists(cgrvdb);
		getCurrentTime(&(cgrvdb->lastLoadTime));
	}

	terminusNode = findNode(ionvdb, terminusNodeNbr, &nextNode);
	if (terminusNode == NULL)
	{
		TRACE(CgrInvalidTerminusNode);

		return 0;	/*	Can't apply CGR.		*/
	}

	ionMemIdx = getIonMemoryMgr();
	proximateNodes = lyst_create_using(ionMemIdx);
	excludedNodes = lyst_create_using(ionMemIdx);
	if (proximateNodes == NULL || excludedNodes == NULL)
	{
		putErrmsg("Can't create lists for route computation.", NULL);
		return -1;
	}

	lyst_delete_set(proximateNodes, deleteObject, NULL);
	lyst_delete_set(excludedNodes, deleteObject, NULL);
	if (!bundle->returnToSender)
	{
		/*	Must exclude sender of bundle from consideration
		 *	as a station on the route, to minimize routing
		 *	loops.  If returnToSender is 1 then we are
		 *	re-routing, possibly back through the sender,
		 *	because we have hit a dead end in routing and
		 *	must backtrack.					*/

		if (excludeNode(excludedNodes, bundle->clDossier.senderNodeNbr))
		{
			putErrmsg("Can't exclude sender from routes.", NULL);
			lyst_destroy(excludedNodes);
			lyst_destroy(proximateNodes);
			return -1;
		}
	}

	/*	Insert into the excludedNodes list all neighbors that
	 *	have been refusing custody of bundles destined for the
	 *	destination node.					*/

	for (embElt = sm_list_first(ionwm, terminusNode->embargoes);
			embElt; embElt = sm_list_next(ionwm, embElt))
	{
		embargo = (Embargo *) psp(ionwm, sm_list_data(ionwm, embElt));
		if (!(embargo->probeIsDue))
		{
			/*	(Omit the embargoed node from the list
			 *	of excluded nodes if it's now time to
			 *	probe that node for renewed acceptance
			 *	of bundles destined for this destination
			 *	node.)					*/

			if (excludeNode(excludedNodes, embargo->nodeNbr))
			{
				putErrmsg("Can't note embargo.", NULL);
				lyst_destroy(excludedNodes);
				lyst_destroy(proximateNodes);
				return -1;
			}
		}
	}

	/*	Consult the contact graph to identify the neighboring
	 *	node(s) to forward the bundle to.			*/

	if (identifyProximateNodes(terminusNode, bundle, bundleObj,
			excludedNodes, trace, proximateNodes, atTime) < 0)
	{
		putErrmsg("Can't identify proximate nodes for bundle.", NULL);
		lyst_destroy(excludedNodes);
		lyst_destroy(proximateNodes);
		return -1;
	}

	/*	Examine the list of proximate nodes.  If the bundle is
	 *	critical, enqueue it on the plan for EACH identified
	 *	proximate receiving node.
	 *
	 *	Otherwise, enqueue the bundle on the plan for the
	 *	most preferred identified proximate receiving node.	*/

	lyst_destroy(excludedNodes);
	TRACE(CgrSelectProximateNodes);
	if (bundle->ancillaryData.flags & BP_MINIMUM_LATENCY)
	{
		/*	Critical bundle; send to all capable neighbors.	*/

		TRACE(CgrUseAllProximateNodes);
		return sendCriticalBundle(bundle, bundleObj, terminusNode,
				proximateNodes, preview);
	}

	/*	Non-critical bundle; send to the most preferred
	 *	neighbor.						*/

	selectedNeighbor = NULL;
	for (elt = lyst_first(proximateNodes); elt; elt = nextElt)
	{
		nextElt = lyst_next(elt);
		proxNode = (ProximateNode *) lyst_data_set(elt, NULL);
		lyst_delete(elt);
		TRACE(CgrConsiderProximateNode, proxNode->neighborNodeNbr);

		/*	Skip this candidate if not cost-effective.	*/

		if (bundle->dlvConfidence > 0.0
		&& bundle->dlvConfidence < 1.0)
		{
			newDlvConfidence =
				getNewDlvConfidence(bundle, proxNode);
			confidenceImprovement =
				(newDlvConfidence / bundle->dlvConfidence)
				- 1.0;
			if (confidenceImprovement < MIN_CONFIDENCE_IMPROVEMENT)
			{
				TRACE(CgrIgnoreProximateNode, CgrNoHelp);
				MRELEASE(proxNode);
				continue;
			}
		}

		/*	Select this candidate if it's the best.		*/

		if (selectedNeighbor == NULL)	/*	1st candidate.	*/
		{
			TRACE(CgrSelectProximateNode);
			selectedNeighbor = proxNode;
		}
		else if (proxNode->arrivalTime <
				selectedNeighbor->arrivalTime)
		{
			TRACE(CgrSelectProximateNode);
			MRELEASE(selectedNeighbor);
			selectedNeighbor = proxNode;
		}
		else if (proxNode->arrivalTime >
				selectedNeighbor->arrivalTime)
		{
			TRACE(CgrIgnoreProximateNode, CgrLaterArrivalTime);
			MRELEASE(proxNode);
		}
		else if (proxNode->hopCount < selectedNeighbor->hopCount)
		{
			TRACE(CgrSelectProximateNode);
			MRELEASE(selectedNeighbor);
			selectedNeighbor = proxNode;
		}
		else if (proxNode->hopCount > selectedNeighbor->hopCount)
		{
			TRACE(CgrIgnoreProximateNode, CgrMoreHops);
			MRELEASE(proxNode);
		}
		else if (proxNode->forfeitTime > selectedNeighbor->forfeitTime)
		{
			TRACE(CgrSelectProximateNode);
			MRELEASE(selectedNeighbor);
			selectedNeighbor = proxNode;
		}
		else if (proxNode->forfeitTime < selectedNeighbor->forfeitTime)
		{
			TRACE(CgrIgnoreProximateNode, CgrEarlierTermination);
			MRELEASE(proxNode);
		}
		else if (proxNode->neighborNodeNbr <
				selectedNeighbor->neighborNodeNbr)
		{
			TRACE(CgrSelectProximateNode);
			MRELEASE(selectedNeighbor);
			selectedNeighbor = proxNode;
		}
		else	/*	Larger node#; ignore.	*/
		{
			TRACE(CgrIgnoreProximateNode, CgrLargerNodeNbr);
			MRELEASE(proxNode);
		}
	}

	lyst_destroy(proximateNodes);
	if (selectedNeighbor)
	{
		TRACE(CgrUseProximateNode, selectedNeighbor->neighborNodeNbr);
		if (!preview)
		{
			if (enqueueToNeighbor(selectedNeighbor, bundle,
					bundleObj, terminusNode))
			{
				putErrmsg("Can't queue for neighbor.", NULL);
				return -1;
			}

#if (MANAGE_OVERBOOKING == 1)
			/*	Handle any contact overbooking caused
			 *	by enqueuing this bundle.		*/

			if (manageOverbooking(selectedNeighbor, bundle, trace))
			{
				putErrmsg("Can't manage overbooking", NULL);
				return -1;
			}
#endif
		}

		MRELEASE(selectedNeighbor);
	}
	else
	{
		TRACE(CgrNoProximateNode);
	}

	if (bundle->dlvConfidence >= MIN_NET_DELIVERY_CONFIDENCE
	|| bundle->id.source.c.nodeNbr == bundle->destination.c.nodeNbr)
	{
		return 0;	/*	Potential future fwd unneeded.	*/
	}

	routes = terminusNode->routingObject;
	if (routes == 0 || sm_list_length(getIonwm(), routes) == 0)
	{
		return 0;	/*	No potential future forwarding.	*/
	}

	/*	Must put bundle in limbo, keep on trying to send it.	*/

	if (bundle->planXmitElt)
	{
		/*	This copy of bundle has already been enqueued.	*/

		if (bpClone(bundle, &newBundle, &newBundleObj, 0, 0) < 0)
		{
			putErrmsg("Can't clone bundle.", NULL);
			return -1;
		}

		bundle = &newBundle;
		bundleObj = newBundleObj;
	}

	if (enqueueToLimbo(bundle, bundleObj) < 0)
	{
		putErrmsg("Can't put bundle in limbo.", NULL);
		return -1;
	}

	return 0;
}

int	cgr_preview_forward(Bundle *bundle, Object bundleObj,
		uvast terminusNodeNbr, time_t atTime, CgrTrace *trace)
{
	if (cgrForward(bundle, bundleObj, terminusNodeNbr, atTime, trace, 1)
			< 0)
	{
		putErrmsg("Can't compute route.", NULL);
		return -1;
	}

	return 0;
}

int	cgr_forward(Bundle *bundle, Object bundleObj, uvast terminusNodeNbr,
		CgrTrace *trace)
{
	if (cgrForward(bundle, bundleObj, terminusNodeNbr, getUTCTime(), trace,
			0) < 0)
	{
		putErrmsg("Can't compute route.", NULL);
		return -1;
	}

	return 0;
}

float	cgr_prospect(uvast terminusNodeNbr, unsigned int deadline)
{
	PsmPartition	wm = getIonwm();
	IonVdb		*ionvdb = getIonVdb();
	time_t		currentTime = getUTCTime();
	IonNode		*terminusNode;
	PsmAddress	nextNode;
	PsmAddress	routes;		/*	SmList of CgrRoutes.	*/
	PsmAddress	elt;
	PsmAddress	addr;
	CgrRoute	*route;
	float		prospect = 0.0;

	terminusNode = findNode(ionvdb, terminusNodeNbr, & nextNode);
	if (terminusNode == NULL)
	{
		return 0.0;		/*	Unknown node, no chance.*/
	}

	routes = terminusNode->routingObject;
	if (routes == 0)
	{
		return 0.0;		/*	No routes, no chance.	*/
	}

	for (elt = sm_list_first(wm, routes); elt; elt = sm_list_next(wm, elt))
	{
		addr = sm_list_data(wm, elt);
		route = (CgrRoute *) psp(wm, addr);
		if (route->toTime < currentTime)
		{
			continue;	/*	Obsolete route.		*/
		}

		if (route->arrivalTime > deadline)
		{
			continue;	/*	Not a plausible route.	*/
		}

		if (route->arrivalConfidence > prospect)
		{
			prospect = route->arrivalConfidence;
		}
	}

	return prospect;
}

void	cgr_start()
{
	oK(getCgrVdb());
}

const char	*cgr_tracepoint_text(CgrTraceType traceType)
{
	int			i = traceType;
	static const char	*tracepointText[] =
	{
	[CgrBuildRoutes] = "BUILD terminusNode:" UVAST_FIELDSPEC
		" payloadLength:%u atTime:%u",
	[CgrInvalidTerminusNode] = "    INVALID terminus node",

	[CgrBeginRoute] = "  ROUTE",
	[CgrConsiderRoot] = "    ROOT fromNode:" UVAST_FIELDSPEC
		" toNode:" UVAST_FIELDSPEC,
	[CgrConsiderContact] = "      CONTACT fromNode:" UVAST_FIELDSPEC
		" toNode:" UVAST_FIELDSPEC,
	[CgrIgnoreContact] = "        IGNORE",

	[CgrCost] = "        COST transmitTime:%u owlt:%u arrivalTime:%u",
	[CgrHop] = "    HOP fromNode:" UVAST_FIELDSPEC " toNode:"
		UVAST_FIELDSPEC,

	[CgrAcceptRoute] = "    ACCEPT firstHop:" UVAST_FIELDSPEC
		" fromTime:%u arrivalTime:%u maxCapacity:" UVAST_FIELDSPEC,
	[CgrDiscardRoute] = "    DISCARD route",

	[CgrIdentifyProximateNodes] = "IDENTIFY deadline:%u",
	[CgrCheckRoute] = "  CHECK firstHop:" UVAST_FIELDSPEC
		" fromTime:%u arrivalTime:%u",
	[CgrRecomputeRoute] = "  RECOMPUTE",
	[CgrIgnoreRoute] = "    IGNORE",

	[CgrAddProximateNode] = "    ADD",
	[CgrUpdateProximateNode] = "    UPDATE",

	[CgrSelectProximateNodes] = "SELECT",
	[CgrUseAllProximateNodes] = "  USE all proximate nodes",
	[CgrConsiderProximateNode] = "  CONSIDER " UVAST_FIELDSPEC,
	[CgrSelectProximateNode] = "    SELECT",
	[CgrIgnoreProximateNode] = "    IGNORE",
	[CgrUseProximateNode] = "  USE " UVAST_FIELDSPEC,
	[CgrNoProximateNode] = "  NO proximate node",
	[CgrFullOverbooking] = "	Full OVERBOOKING (amount in bytes):%f",
	[CgrPartialOverbooking] = " Partial OVERBOOKING (amount in bytes):%f",
	};

	if (i < 0 || i >= CgrTraceTypeMax)
	{
		return "";
	}

	return tracepointText[i];
}

const char	*cgr_reason_text(CgrReason reason)
{
	int			i = reason;
	static const char	*reasonText[] =
	{
	[CgrContactEndsEarly] = "contact ends before data arrives",
	[CgrSuppressed] = "contact is suppressed",
	[CgrVisited] = "contact has been visited",
	[CgrNoRange] = "no range for contact",

	[CgrRouteViaSelf] = "route is via self",
	[CgrRouteCapacityTooSmall] = "route includes a contact that's too \
small for this bundle",
	[CgrInitialContactExcluded] = "first node on route is an excluded \
neighbor",
	[CgrRouteTooSlow] = "route is too slow; radiation latency delays \
arrival time too much",
	[CgrNoPlan] = "no egress plan",
	[CgrBlockedPlan] = "egress plan is blocked",
	[CgrMaxPayloadTooSmall] = "max payload too small",
	[CgrNoResidualCapacity] = "contact with this neighbor is already \
fully subscribed",
	[CgrResidualCapacityTooSmall] = "too little residual aggregate \
capacity for this bundle",

	[CgrMoreHops] = "more hops",
	[CgrEarlierTermination] = "earlier route termination time",
	[CgrNoHelp] = "insufficient delivery confidence improvement",
	[CgrLowerCapacity] = "lower path capacity",
	[CgrLaterArrivalTime] = "later arrival time",
	[CgrLargerNodeNbr] = "initial hop has larger node number",
	};

	if (i < 0 || i >= CgrReasonMax)
	{
		return "";
	}

	return reasonText[i];
}

void	cgr_stop()
{
	PsmPartition	wm = getIonwm();
	char		*name = "cgrvdb";
	PsmAddress	vdbAddress;
	PsmAddress	elt;
	CgrVdb		*vdb;

	/*Clear Route Caches*/
	clearRoutingObjects(wm);

	/*Free volatile database*/
	if (psm_locate(wm, name, &vdbAddress, &elt) < 0)
	{
		putErrmsg("Failed searching for vdb.", NULL);
		return;
	}

	if (elt)
	{
		vdb = (CgrVdb *) psp(wm, vdbAddress);
		sm_list_destroy(wm, vdb->routeLists, NULL, NULL);
		psm_free(wm, vdbAddress);
		if (psm_uncatlg(wm, name) < 0)
		{
			putErrmsg("Failed Uncataloging vdb.",NULL);
		}
	}
}
