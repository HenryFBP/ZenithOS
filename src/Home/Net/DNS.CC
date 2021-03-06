//#include "UDP";

// https://www2.cs.duke.edu/courses/fall16/compsci356/DNS/DNS-primer.pdf
// https://en.wikipedia.org/wiki/Domain_Name_System

// DNS Cache is a HashTable, similar to ARP Cache

#define DNS_HASHTABLE_SIZE	2048	// 1024 might be fine, test it
#define HTT_DNS				0x00100 // identical to HTT_DICT_WORD

#define DNS_FLAG_RD		0x0100

#define DNS_OP_QUERY	0

#define DNS_TYPE_A		1

#define DNS_CLASS_IN	1

#define DNS_TIMEOUT		5000

#define DNS_MAX_RETRIES	5	// Shrine has 3, why not 5? :^)

class CDNSHash:CHash
{	// store U8 *hostname as CHash->str U8 *
	CAddressInfo info;
	// Shrine has 'TODO: honor TTL' ...
	// Duke: 'TTL: the number of seconds the results can be cached'
	// perhaps have a separate task for removing cached results ?
};

class CDNSDomainName
{
	U8 **labels;
	I64  num_labels;
};

class CDNSQuestion
{
	CDNSQuestion	*next;
	CDNSDomainName	 q_name;
	U16				 q_type;
	U16				 q_class;
};

class CDNSHeader
{
	U16 id;
	U16 flags;
	U16 q_count;	// number of entries in question section
	U16 a_count;	// number of resource records in answer section
	U16 ns_count;	// number of name server resource records in authority records section
	U16 ar_count;	// number of resource records in additional records section
};

class CDNSRR
{ // RR: Resource Record
	CDNSRR			*next;
	CDNSDomainName	 name;		// name of the node this record is for
	U16				 type;		// RR type, e.g. 44=SSHFP, 15=MX, 49=DHCID ...
	U16				 rr_class;	// class code
	U32				 ttl;		// count in seconds that RR stays valid (max = 2^31 - 1)
	U16				 rd_length;	// length of r_data member
	U8				*r_data;	// additional RR-specific data
};

class CDNSGlobals
{
	U16					addr_family;
	CIPAddressStorage	dns_ip;

} dns_globals;

CHashTable *dns_cache = NULL;

U0 DNSCacheInit()
{
	dns_cache = HashTableNew(DNS_HASHTABLE_SIZE);

	MemSet(&dns_globals.dns_ip, 0, sizeof(CIPAddressStorage));
	dns_globals.addr_family = 0;
}

CDNSHash *DNSCacheFind(U8 *hostname)
{
	CDNSHash *entry = HashFind(hostname, dns_cache, HTT_DNS);

	if (entry == NULL)
		ZenithWarn("DNS CACHE FIND: Could not find a hostname in the DNS Cache.\n");

	return entry;
}

CDNSHash *DNSCachePut(U8 *hostname, CAddressInfo *info)
{
	ZenithLog("DNS CACHE PUT: Attempting Find DNS Entry in Cache: hostname: %s\n", hostname);
/*	"==\n";ClassRep(info);
	ClassRep(info->address(CSocketAddressIPV4 *));"==\n";*/
	CDNSHash *entry = DNSCacheFind(hostname);

	if (!entry)
	{
		entry = CAlloc(sizeof(CDNSHash));
		entry->str	= StrNew(hostname);
		entry->type = HTT_DNS;

		AddressInfoCopy(&entry->info, info);

		HashAdd(entry, dns_cache);
	}
	else
		ZenithWarn("DNS CACHE PUT: Entry was already found in Cache. TODO: overwrite?");

	return entry;
}

I64 DNSCalculateQuestionSize(CDNSQuestion *q)
{ // ??
	I64 i, size = 0;

	for (i = 0; i < q->q_name.num_labels; i++)
	{
		size += 1 + StrLen(q->q_name.labels[i]);
	}

	return size + 1 + 4;
}

U0 DNSSerializeQuestion(U8 *buffer, CDNSQuestion *q)
{ // ??
	I64 i;
	U8 *label;

	for (i = 0; i < q->q_name.num_labels; i++)
	{
		label = q->q_name.labels[i];

		*(buffer++) = StrLen(label);

		while (*label)
			*(buffer++) = *(label++);
	}

	*(buffer++) = 0;
	*(buffer++) = q->q_type >> 8;
	*(buffer++) = q->q_type & 0xFF;
	*(buffer++) = q->q_class >> 8;
	*(buffer++) = q->q_class & 0xFF;
}

I64 DNSSendQuestion(U16 id, U16 local_port, CDNSQuestion *q)
{
	CIPV4Address* ipv4_addr;
	U8 *frame;
	U16 flags;
	CDNSHeader *header;
	I64 de_index;

	switch (dns_globals.addr_family)
	{
		case AF_UNSPEC: // 0, global dns ip not set
			ZenithErr("DNS SEND QUESTION: Failed, global dns addr family was AF_UNSPEC.\n");
			return -1;

		case AF_INET6:
			ZenithErr("DNS SEND QUESTION: Failed, IPV6 not supported yet in DNS.\n");
			throw('DNS');

		case AF_INET:
			ipv4_addr = &dns_globals.dns_ip;

			if (!*ipv4_addr)
			{
				ZenithErr("DNS SEND QUESTION: Failed, ipv4_addr had no value set.\n");
				return -1;
			}
	}

	// UDPPacketAllocate currently only accepts IPV4 ...
	de_index = UDPPacketAllocate(&frame,
								 IPV4GetAddress(),
								 local_port,
								 *ipv4_addr,
								 53,
								 sizeof(CDNSHeader) + DNSCalculateQuestionSize(q));
	if (de_index < 0)
	{
		ZenithErr("DNS SEND QUESTION: Failed, UDPPacketAllocate returned error.\n");
		return de_index;
	}

	flags = (DNS_OP_QUERY << 11) | DNS_FLAG_RD;

	header = frame;

	header->id			= EndianU16(id);
	header->flags		= EndianU16(flags);
	header->q_count		= EndianU16(1);
	header->a_count		= 0;
	header->ns_count	= 0;
	header->ar_count	= 0;

	DNSSerializeQuestion(frame + sizeof(CDNSHeader), q);

	UDPPacketFinish(de_index);
	return 0;

}


I64 DNSParseDomainName(U8 *packet_data, I64 packet_length, U8 **data_inout, I64 *length_inout, CDNSDomainName *name_out)
{ // these methods look not-so-good, ngl.
	U8 *data = *data_inout;
	U8 *name_buf;
	I64 length = *length_inout;
	I64 label_len;
	Bool jump_taken = FALSE;

	if (length < 1)
	{
		ZenithErr("DNS PARSE DOMAIN NAME: Length less than one.\n");
		return -1;
	}

	name_out->labels		= CAlloc(16 * sizeof(U8 *));
	name_out->num_labels	= 0;

	name_buf = CAlloc(256); // ?..
	name_out->labels[0] = name_buf;

	while (length)
	{
		label_len = *(data++);
		length--;

		if (label_len == 0)
			break;
		else if (label_len >= 192)
		{
			label_len &= 0x3F; // ...

			if (!jump_taken)
			{
				*data_inout		= data   + 1;
				*length_inout	= length - 1;
				jump_taken		= TRUE;
				ZenithLog("DNS PARSE DOMAIN NAME: Jump taken\n");
			}

			data	= packet_data + ((label_len << 8) | *data);
			length	= packet_data + packet_length - data;
		}
		else
		{
			if (length < label_len)
				return -1; // ?

			MemCopy(name_buf, data, label_len);
			data	+= label_len;
			length	-= label_len;

			name_buf[label_len] = 0;
			name_out->labels[name_out->num_labels++] = name_buf;

			name_buf += label_len + 1;
		}
	}

	if (!jump_taken)
	{
		*data_inout		= data;
		*length_inout	= length;
	}

	return 0;
}


I64 DNSParseQuestion(U8 *packet_data, I64 packet_length, U8 **data_inout, I64 *length_inout, CDNSQuestion *q_out)
{
	U8 *data;
	I64 length;
	I64 error = DNSParseDomainName(packet_data, packet_length, data_inout, length_inout, &q_out->q_name);

	if (error < 0)
		return error;

	data	= *data_inout;
	length	= *length_inout;

	if (length < 4)
		return -1;

	q_out->next		= NULL;
	q_out->q_type	= (data[1] << 8) | data[0];
	q_out->q_class	= (data[3] << 8) | data[2];

	*data_inout		= data   + 4;
	*length_inout	= length - 4;

	return 0;
}

I64 DNSParseRR(U8 *packet_data, I64 packet_length, U8 **data_inout, I64 *length_inout, CDNSRR *rr_out)
{
	U8 *data;
	I64 length;
	I64 record_length;
	I64 error = DNSParseDomainName(packet_data, packet_length, data_inout, length_inout, &rr_out->name);

	if (error < 0)
		return error;

	data	= *data_inout;
	length	= *length_inout;

	if (length < 10)
		return -1;

	rr_out->next = NULL;
	MemCopy(&rr_out->type, data, 10); // ???

	record_length = 10 + EndianU16(rr_out->rd_length);

	if (length < record_length)
		return -1;

	rr_out->r_data = data + 10; // ??

	*data_inout		= data   + record_length;
	*length_inout	= length - record_length;

	return 0;
}

I64 DNSParseResponse(U16 id, U8 *data, I64 length, CDNSHeader **header_out, CDNSQuestion **questions_out, CDNSRR **answers_out)
{
	CDNSHeader		*header;
	CDNSQuestion	*question;
	CDNSRR			*answer;
	I64				 i;
	U8				*packet_data	= data;
	I64				 packet_length	= length;

	if (length < sizeof(CDNSHeader))
	{
		ZenithErr("DNS PARSE RESPONSE: Length too short.\n");
		return -1;
	}

	header = data;
	data += sizeof(CDNSHeader);

	if (id != 0 && EndianU16(header->id) != id)
	{
		ZenithErr("DNS PARSE RESPONSE: Header ID mismatch.\n");
		return -1;
	}

	for (i = 0; i < EndianU16(header->q_count); i++)
	{
		question = CAlloc(sizeof(CDNSQuestion));
		if (DNSParseQuestion(packet_data, packet_length, &data, &length, question) < 0)
			return -1;

		question->next = *questions_out;
		*questions_out = question;
	}

	for (i = 0; i < EndianU16(header->a_count); i++)
	{
		answer = CAlloc(sizeof(CDNSRR));
		if (DNSParseRR(packet_data, packet_length, &data, &length, answer) < 0)
			return -1;

		answer->next = *answers_out;
		*answers_out = answer;
	}

	*header_out = header;
	return 0;

}

U0 DNSBuildQuestion(CDNSQuestion *q, U8 *name)
{
	U8 *copy = StrNew(name);
	U8 *dot;

	q->next					= NULL;
	q->q_name.labels		= CAlloc(16 * sizeof(U8 *));
	q->q_name.labels[0]		= 0;
	q->q_name.num_labels	= 0;
	q->q_type				= DNS_TYPE_A;
	q->q_class				= DNS_CLASS_IN;

	while (*copy)
	{
		q->q_name.labels[q->q_name.num_labels++] = copy;
		dot = StrFirstOcc(copy, ".");

		if (dot)
		{
			*dot = 0;
			copy = dot + 1;
		}
		else
			break;
	}
}

// these Free methods bother me a bit...
U0 DNSFreeQuestion(CDNSQuestion *q)
{
	Free(q->q_name.labels[0]);
}

U0 DNSFreeRR(CDNSRR *rr)
{
	Free(rr->name.labels[0]);
}

U0 DNSFreeQuestionChain(CDNSQuestion *questions)
{
	CDNSQuestion *next;

	while (questions)
	{
		next = questions->next;
		DNSFreeQuestion(questions);
		Free(questions);
		questions = next;
	}
}

U0 DNSFreeRRChain(CDNSRR *rrs)
{ // Shrine sets rrs->next to a CDNSQuestion when it would be a CDNSRR ... assuming it's wrong and fixing it here..
	CDNSRR *next;

	while (rrs)
	{
		next = rrs->next;
		DNSFreeRR(rrs);
		Free(rrs);
		rrs = next;
	}
}


I64 DNSRunQuery(CUDPSocket *udp_socket, U8 *name, U16 port, CAddressInfo **result_out)
{	// IPV4-UDP-based, TODO: take good look at this method to ensure no floating pointers after.
	// note: UDP Socket created in this method is not closed in this method, gets closed e.g. in DNSGetAddressInfo
	I64  retries	= 0;
	I64	 timeout	= DNS_TIMEOUT;
	U16	 local_port	= RandU16;	// TODO: is rand needed? would a local port 0 work? (would improve lookup speed)
	U16  id			= RandU16;
	I64  error		= 0;
	U8   buffer[2048];
	I64  count;
	Bool have;	// ??

	CDNSQuestion	 q;
	CDNSHeader		*header;
	CDNSQuestion	*questions;
	CDNSRR			*answers;
	CDNSRR			*a;

	CSocketAddressIPV4	 ipv4_addr;
	CSocketAddressIPV4	 ipv4_addr_in; // ?
	CSocketAddressIPV4	*ipv4_addr_temp;
	CAddressInfo		*res;

	//setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO_MS, &timeout, sizeof(timeout))
	udp_socket->receive_timeout_ms = timeout;

	ipv4_addr.family			= AF_INET;
	ipv4_addr.port				= EndianU16(local_port);
	ipv4_addr.address.address	= INADDR_ANY;

	// UDPSocketBind will be attempted on the udp_socket param, method expects a UDPSocket() result to be made already
	if (UDPSocketBind(udp_socket, &ipv4_addr)) // expected return value is 0
	{
		ZenithErr("DNS RUN QUERY: Failed to bind UDP socket.\n");
		return -1;
	}

	DNSBuildQuestion(&q, name);

	while (TRUE) // Shrine uses while (1) infinite loop, need to be careful not to lock
	{
		error = DNSSendQuestion(id, local_port, &q);
		if (error < 0)
		{
			ZenithErr("DNS RUN QUERY: Failed to Send Question.\n");
			return -1;
		}

		count = UDPSocketReceiveFrom(udp_socket, buffer, sizeof(buffer), &ipv4_addr_in);

		if (count > 0)
		{
			ZenithLog("DNS RUN QUERY: Trying Parse Response.\n");

			header		= NULL;
			questions	= NULL;
			answers		= NULL;

			error = DNSParseResponse(id, buffer, count, &header, &questions, &answers);

			if (error == 0) // Shrine has (error >= 0), but DNSParseResponse can only return 0 or 1 ..
			{
				have = FALSE;

				a = answers;
				while (a)
				{
					// Shrine has TODO: if multiple acceptable answers, pick one at random, not just first one.
					// perhaps we could use r_count in header for that ?

					if (EndianU16(a->type)		== DNS_TYPE_A	&&
						EndianU16(a->rr_class)	== DNS_CLASS_IN &&
						EndianU16(a->rd_length)	== 4)
					{
						res = CAlloc(sizeof(CAddressInfo));

						res->flags			= 0;
						res->family			= AF_INET;
						res->socket_type	= 0;	// ??
						res->protocol		= 0;	// ??
						res->address_length	= sizeof(CSocketAddressIPV4);
						res->address		= CAlloc(sizeof(CSocketAddressIPV4));
						res->canonical_name	= 0;
						res->next			= NULL;

						ipv4_addr_temp = res->address;

						ipv4_addr_temp->family	= AF_INET;
						ipv4_addr_temp->port	= port;
						MemCopy(&ipv4_addr_temp->address.address, answers->r_data, 4);

						DNSCachePut(name, res);
						*result_out = res;
						have = TRUE;

						break;
					}

					a = a->next;
				}

				DNSFreeQuestionChain(questions);
				DNSFreeRRChain(answers);

				if (have)
					break;

				// Shrine comment: 'at this point, we could try iterative resolution,
				// but all end-user DNS servers would have tried that already'

				ZenithErr("DNS RUN QUERY: Failed to find suitable answer in reply.\n");
				error = -1;
			}
			else
			{
				ZenithErr("DNS RUN QUERY: Failed a DNS Parse Response.\n");
			}
		}

		if (++retries == DNS_MAX_RETRIES)
		{
			ZenithErr("DNS RUN QUERY: Failed, max retries reached.\n");
			error = -1;
			break;
		}
	}

	DNSFreeQuestion(&q);
	return error;
}

// Shrine has port arg as U8 *service with a no_warn and says it should be parsed as port, allowing that here
// Also has CAddressInfo *hints with a no_warn, omitting that for now
I64 DNSGetAddressInfo(U8 *node_name, U16 port, CAddressInfo **result)
{
	I64			 error;
	CUDPSocket	*udp_socket;
	CDNSHash	*cached_entry = DNSCacheFind(node_name);

	if (cached_entry)
	{
		*result = CAlloc(sizeof(CAddressInfo));
		AddressInfoCopy(*result, &cached_entry->info);
		//(*res)->flags |= AI_CACHED; // TODO: add AI_CACHED define (maybe a better name?) not used anywhere i don't think..
		return 0;
	}

	udp_socket = UDPSocket(AF_INET);
	error = 0;

	if (udp_socket)
	{
		error = DNSRunQuery(udp_socket, node_name, port, result);

		UDPSocketClose(udp_socket);
	}
	else
	{
		ZenithErr("DNS GET ADDRESS INFO: Failed to make UDP Socket.\n");
		error = -1;
	}

	return error;
}

U0 DNSSetResolverIPV4(U32 ip)
{
	CIPV4Address *address = &dns_globals.dns_ip;

	dns_globals.addr_family	= AF_INET;
	address->address		= ip;
}

U0 Host(U8 *hostname)
{ // getaddrinfo() for whole system in Shrine ends up as pointer to DNSGetAddressInfo.. should we do something similar?
	CAddressInfo	*current;
	CAddressInfo	*result	= NULL;
	I64				 error	= DNSGetAddressInfo(hostname, NULL, &result);
	I64				 i = 0;

	if (error < 0)
	{
		ZenithErr("HOST(): Failed at DNS Get Address Info.\n");
	}
	else
	{
		"Host() Results:\n\n";
		current = result;
		while (current)
		{
			"Result %d:\n", ++i;

			"	flags:			%04Xh \n", current->flags;
			"	family:			%d    \n", current->family;
			"	socket type:	%d    \n", current->socket_type;
			"	protocol:		%d    \n", current->protocol;
			"	address length:	%d    \n", current->address_length;
			"	address:		%s    \n", NetworkToPresentation(AF_INET, &current->address(CSocketAddressIPV4 *)->address);

			current = current->next;
		}
	}

	AddressInfoFree(result);
}

U0 DNSRep()
{ // TODO: primitive, needs refine. switch() on Socket Address types for NetworkToPresentation, etc
	I64			 i;
	CDNSHash	*temp_hash;

	"\n";
	Who(, dns_cache);
	"\n";

	for (i = 0; i <= dns_cache->mask; i++)
	{
		temp_hash = dns_cache->body[i];

		while (temp_hash)
		{
			ClassRep(temp_hash,, 5);
			temp_hash = temp_hash->next;
		}
	}
}

/*

U0 DNSInit()

*/

DNSCacheInit;