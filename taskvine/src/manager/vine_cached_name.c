/*
Copyright (C) 2023- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include "vine_file.h"
#include "vine_task.h"
#include "vine_protocol.h"
#include "vine_checksum.h"

#include "stringtools.h"
#include "md5.h"
#include "debug.h"
#include "xxmalloc.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

/*
For a given task and file, generate the name under which the file
should be stored in the remote cache directory.

The basic strategy is to construct a name that is unique to the
namespace from where the file is drawn, so that tasks sharing
the same input file can share the same copy.

In the common case of files, the cached name is based on the
hash of the local path, with the basename of the local path
included simply to assist with debugging.

In each of the other file types, a similar approach is taken,
including a hash and a name where one is known, or another
unique identifier where no name is available.
*/

/*
For a given file we want the generation of a cachename for said file to be injective.
This ensures the files cached at the worker are the exact files we need.

For each file type a different strategy must be used to generate the cachenames for that file.
Given that files can generally have the same name across namespaces solely using the filename is
not adequate when generating cachenames.

Preferably cachenames would always be generated using data relevant to the contents of the file.
However, this is not always available. The following discusses methods for generating cachenames
for each file type:

VINE_BUFFER - With buffers, the content of the buffer is available to us we can then use an adequate
hashing function on the contents of the buffer.

VINE_FILE - With local files, Assuming we have permission to read the given file, the contents of
the file are available for us to hash. However, due to the variable sizes of files and the number of files
that may need to be hashed, hashing can cause an unwanted amount of overhead. However, different hashing methods
can generate different changes to our overhead to be more favorable. Furthermore, it is important that any method chose
is consistent and avoids conflicts adequately. For directories, which are a subset of the VINE_FILE classification,
it is important that the directory is hashed from its contents. This can be done by using a variation of a merkle tree.
That is, each hash of a directory is a hash of the hashes of the files with the directory. This can be done recursively.

VINE_EMPTY_DIR - Are there cases where an empty directory needs to be unique?

VINE_URL - With files possibly hosted on remote machines, We generally dont have access to the contents unless one transfers
the entire file to the site of the manager which is somewhat antithetical to the use case for VINE_URLs. Here, our general strategy
is to only retrieve the header of the file from the server. With the information in the header, some fields can give us insights
to the identity of the file. More on HTTP header fields: https://www.rfc-editor.org/rfc/rfc4229#section-2.1.24
Once, the header is retrieved, fields such as Content-MD5, ETag, and Last-Modified can be used to generate the cachenames.
The following on are details on each header field used:

    	'Content-MD5' - This is an md5 digest of the entity, This field could be generated by an origin server or a client.
    	https://www.rfc-editor.org/rfc/rfc2616#page-121

    	'ETag' - an ETag or entity-tag is an "opaque" cache validator. Typically used to validate changes for a given resource.
    	There is no specification on how an ETag can be generated on a server. It could be a hash of the content, but this is not always the case.
    	ETags that begin with W/ indicate that a weak validator was used to generate the ETag.
    	https://www.rfc-editor.org/rfc/rfc2616#page-126
    	https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/ETag

    	'Last-Modified' - This is the date and time the a resource was last changed on a server
    	https://www.rfc-editor.org/rfc/rfc2616#page-89

We then generate a hierarchy of header-fields that is equivalent to the order as they appear above. The reasoning is as follows: With md5 hashes
We can determine that two files with the same hash are the same. For ETags, we can be confident that two files are the same IF they are from the same server.
This follows for Las-Modified timestamps but an extra piece of information is needed to generate the cachename. For each header retrieved we opt for the
field that is highest on the hierarchy when present. For Last-Modified, we need an additional field to generate a cachename(as two files
can have identical last-modified dates). Currently, this is the url for the given file in addition to the server where the file is to be retrieved. For each field,
each bit of necessary information can be combined together to generate the hash.


VINE_MINITASK - A minitask is the resulting file after executing a given command on the worker. At times these commands have their own file
dependencies which have their own cachenames. There is the  possibility of generating the cachename for the minitask from the cachenames of the files
that the mini task depends on. However, certain commands have a level of dynanicism in which it we cannot use this method to adequately predict the identity
of the resulting file. It could be a possibility to just let the user decide whether a command can have a cachename. However, what happens if they are wrong?

When a cachename cannot be generated - There is an argument that if cachename cannot be generated given the present information, that the file
should not be cached. That is, generating a cachename could lead to possible conflicts on the worker side.
*/

typedef enum {
	VINE_FOUND_NONE,
	VINE_FOUND_LAST_MODIFIED,
	VINE_FOUND_ETAG,
	VINE_FOUND_MD5,
} vine_url_cache_t;

/*
Fetch the headers of a URL, and return the most desirable metadata
value to use for caching purposes: md5 checksum, E-Tag, Last-Modified-Time,
or if all else fails, just the URL itself.
*/

static vine_url_cache_t get_url_properties( const char *url, char *tag )
{
	vine_url_cache_t val = VINE_FOUND_NONE;
	char line[VINE_LINE_MAX];

	/*
	Odd hack: We occasionally use file:// URLs with curl as
	a roundabout way of getting a worker to side-load a file
	from a shared filesystem.  In that case, there is no server
	to get headers from.  Instead, just have the manager checksum directly.
	*/

	if(!strncmp(url,"file://",7)) {
		ssize_t totalsize;
		char *hash = vine_checksum_any(&url[7],&totalsize);
		strcpy(tag,hash);
		free(hash);
		return VINE_FOUND_MD5;
	}

	/* Otherwise, proceed to use curl to get the headers. */

	char *command = string_format("curl -IL --verbose --stderr /dev/stdout \"%s\"",url);

	FILE *stream = popen(command, "r");

	/* If curl itself cannot be executed, then a lot of things won't work. */
	if(!stream) fatal("could not execute \"%s\" : %s",command,strerror(errno));

	while(fgets(line, sizeof(line), stream)) {
		if(sscanf(line, "Content-MD5: %s",tag)){
			val = VINE_FOUND_MD5;
			break;
		}
		if(sscanf(line, "content-md5: %s",tag)){
			val = VINE_FOUND_MD5;
			break;
		}
		if(val < VINE_FOUND_ETAG && sscanf(line, "ETag: %s",tag)){
			val = VINE_FOUND_ETAG;
		}
		if(val < VINE_FOUND_ETAG && sscanf(line, "etag: %s",tag)){
			val = VINE_FOUND_ETAG;
		}
		if(val < VINE_FOUND_LAST_MODIFIED && sscanf(line, "Last-Modified: %s",tag)){
			val = VINE_FOUND_LAST_MODIFIED;
		}
		if(val < VINE_FOUND_LAST_MODIFIED && sscanf(line, "last-modified: %s",tag)){
			val = VINE_FOUND_LAST_MODIFIED;
		}
	}

	int result = pclose(stream);

	/*
	If curl executes but the url cannot be fetched,
	then we cannot just halt here but warn and keep
	going with a hash based on the URL.
	*/

	if(result!=0) {
		debug(D_VINE|D_NOTICE,"Unable to fetch properties of url %s!  Continuing optimistically..",url);
		val = VINE_FOUND_NONE;
	}

	free(command);
	
	return val;
}

/*
The cached name of a url is obtained from the headers provided
by the server.  Ideally, the server provides the md5 checksum
directly.  If not, then we compute from the ETag, Last-Modified-Time,
or if all else fails, from the URL itself.
*/

static char *make_url_cached_name( const struct vine_file *f )
{
	char tag[VINE_LINE_MAX];
	unsigned char digest[MD5_DIGEST_LENGTH];
	char *content;
	const char *hash;
	const char *method;

	debug(D_VINE,"fetching headers for url %s",f->source);

vine_url_cache_t val = get_url_properties(f->source,tag);

	switch(val){
		case VINE_FOUND_NONE:
			/* Checksum the URL alone. */
			method = "md5-url";
			content = string_format("%s",f->source);
			md5_buffer(content,strlen(content),digest);
			hash = md5_to_string(digest);
			free(content);
			break;
		case VINE_FOUND_LAST_MODIFIED:
			/* Checksum the URL and last-modified-time. */
			method = "md5-lm";
			content = string_format("%s-%s",f->source,tag);
			md5_buffer(content,strlen(content),digest);
			hash = md5_to_string(digest);
			free(content);
			break;
		case VINE_FOUND_ETAG:
			/* Checksum the URL and ETag. */
			method = "md5-et";
			content = string_format("%s-%s",f->source,tag);
			md5_buffer(content,strlen(content),digest);
			hash = md5_to_string(digest);
			free(content);
			break;
		case VINE_FOUND_MD5:	
			/* Use the provided checksum of the content. */
			method = "md5-content";
			hash = tag;
			break;
	}

	debug(D_VINE,"using checksum method %s for url %s",method,f->source);
	
	return string_format("%s-%s",method,hash);
}

/*
A mini-task cache name is computed from the hash of:
- The string representation of the task and
- The name of the file extracted from the task.
*/

char *make_mini_task_cached_name(const struct vine_file *f)
{
	unsigned char digest[MD5_DIGEST_LENGTH];

	char *taskstr = vine_task_to_json(f->mini_task);
	char *buffer = string_format("%s:%s",taskstr,f->source);

	md5_buffer(buffer,strlen(buffer),digest);

	free(buffer);
	free(taskstr);

	return strdup(md5_to_string(digest));
}

/*
Compute the cached name of a file object, based on its type.
Returns a string that must be freed with free().
*/

char *vine_cached_name( const struct vine_file *f, ssize_t *totalsize )
{
	unsigned char digest[MD5_DIGEST_LENGTH];
	char *hash, *name;
	char random[17];

	switch(f->type) {
		case VINE_FILE:
			hash = vine_checksum_any(f->source,totalsize);
			if(hash) {
				/* An existing file is identified by its content. */
				name = string_format("file-md5-%s",hash);
				free(hash);
			} else {
				/* A pending file gets a random name. */
				string_cookie(random,16);
				name = string_format("file-rnd-%s",random);
			}
			break;
		case VINE_EMPTY_DIR:
			/* All empty dirs have the same content! */
			name = string_format("empty");
			break;
		case VINE_MINI_TASK:
			/* A mini task is idenfied by the task properties. */
			hash = make_mini_task_cached_name(f);
			name = string_format("task-md5-%s",hash);
			free(hash);
			break;
	       	case VINE_URL:
			/* A url is identified by its metadata. */
			hash = make_url_cached_name(f);
			name = string_format("url-%s",hash);
			free(hash);
			break;
		case VINE_TEMP:
			/* An empty temporary file gets a random name. */
			/* Until we later have a better name for it.*/
			string_cookie(random,16);
			name = string_format("temp-rnd-%s",random);
			break;
		case VINE_BUFFER:
			if(f->data) {
				/* If the buffer exists, then checksum the content. */
				md5_buffer(f->data, f->size, digest);
				const char *hash = md5_to_string(digest);
				name = string_format("buffer-md5-%s",hash);
			} else {
				/* If the buffer doesn't exist yet, then give a random name. */
				/* Until we later have a better name for it.*/
				string_cookie(random,16);
				name = string_format("buffer-rnd-%s",random);
			}
			break;
		default:
			fatal("invalid file type %d",f->type);
			name = strdup("notreached");
			break;
	}

	return name;
}


char *vine_file_id( const struct vine_file *f )
{
	unsigned char digest[MD5_DIGEST_LENGTH];
	const char *hash;

    assert(f->cached_name);

    char *content = string_format("%s%s", f->cached_name, f->source ? f->source : "");
    md5_buffer(content,strlen(content),digest);
    hash = md5_to_string(digest);
    free(content);

    return xxstrdup(hash);
}
