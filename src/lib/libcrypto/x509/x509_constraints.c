/* $OpenBSD: x509_constraints.c,v 1.7 2020/09/20 18:32:33 tb Exp $ */
/*
 * Copyright (c) 2020 Bob Beck <beck@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include <openssl/safestack.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "x509_internal.h"

/* RFC 2821 section 4.5.3.1 */
#define LOCAL_PART_MAX_LEN 64
#define DOMAIN_PART_MAX_LEN 255

struct x509_constraints_name *
x509_constraints_name_new()
{
	return (calloc(1, sizeof(struct x509_constraints_name)));
}

void
x509_constraints_name_clear(struct x509_constraints_name *name)
{
	free(name->name);
	free(name->local);
	free(name->der);
	memset(name, 0, sizeof(*name));
}

void
x509_constraints_name_free(struct x509_constraints_name *name)
{
	if (name == NULL)
		return;
	x509_constraints_name_clear(name);
	free(name);
}

struct x509_constraints_name *
x509_constraints_name_dup(struct x509_constraints_name *name)
{
	struct x509_constraints_name *new;

	if ((new = x509_constraints_name_new()) == NULL)
		goto err;
	new->type = name->type;
	new->af = name->af;
	new->der_len = name->der_len;
	if (name->der_len > 0 && (new->der = malloc(name->der_len)) == NULL)
		goto err;
	memcpy(new->der, name->der, name->der_len);
	if (name->name != NULL && (new->name = strdup(name->name)) == NULL)
		goto err;
	if (name->local != NULL && (new->local = strdup(name->local)) == NULL)
		goto err;
	memcpy(new->address, name->address, sizeof(name->address));
	return new;
 err:
	x509_constraints_name_free(new);
	return NULL;
}

struct x509_constraints_names *
x509_constraints_names_new()
{
	return (calloc(1, sizeof(struct x509_constraints_names)));
}

void
x509_constraints_names_clear(struct x509_constraints_names *names)
{
	size_t i;

	for (i = 0; i < names->names_count; i++)
		x509_constraints_name_free(names->names[i]);
	free(names->names);
	memset(names, 0, sizeof(*names));
}

void
x509_constraints_names_free(struct x509_constraints_names *names)
{
	if (names == NULL)
		return;

	x509_constraints_names_clear(names);
	free(names);
}

int
x509_constraints_names_add(struct x509_constraints_names *names,
    struct x509_constraints_name *name)
{
	size_t i = names->names_count;

	if (names->names_count == names->names_len) {
		struct x509_constraints_name **tmp;
		if ((tmp = recallocarray(names->names, names->names_len,
		    names->names_len + 32, sizeof(*tmp))) == NULL)
			return 0;
		names->names_len += 32;
		names->names = tmp;
	}
	names->names[i] = name;
	names->names_count++;
	return 1;
}

struct x509_constraints_names *
x509_constraints_names_dup(struct x509_constraints_names *names)
{
	struct x509_constraints_names *new = NULL;
	struct x509_constraints_name *name = NULL;
	size_t i;

	if (names == NULL)
		return NULL;

	if ((new = x509_constraints_names_new()) == NULL)
		goto err;
	for (i = 0; i < names->names_count; i++) {
		if ((name = x509_constraints_name_dup(names->names[i])) == NULL)
			goto err;
		if (!x509_constraints_names_add(new, name))
			goto err;
	}
	return new;
 err:
	x509_constraints_names_free(new);
	x509_constraints_name_free(name);
	return NULL;
}


/*
 * Validate that the name contains only a hostname consisting of RFC
 * 5890 compliant A-labels (see RFC 6066 section 3). This is more
 * permissive to allow for a leading '*' for a SAN DNSname wildcard,
 * or a leading '.'  for a subdomain based constraint, as well as
 * allowing for '_' which is commonly accepted by nonconformant
 * DNS implementaitons.
 */
static int
x509_constraints_valid_domain_internal(uint8_t *name, size_t len)
{
	uint8_t prev, c = 0;
	int component = 0;
	int first;
	size_t i;

	if (len > DOMAIN_PART_MAX_LEN)
		return 0;

	for (i = 0; i < len; i++) {
		prev = c;
		c = name[i];

		first = (i == 0);

		/* Everything has to be ASCII, with no NUL byte */
		if (!isascii(c) || c == '\0')
			return 0;
		/* It must be alphanumeric, a '-', '.', '_' or '*' */
		if (!isalnum(c) && c != '-' && c != '.' && c != '_' &&
		    c != '*')
			return 0;

		/* '*' can only be the first thing. */
		if (c == '*' && !first)
			return 0;

		/* '-' must not start a component or be at the end. */
		if (c == '-' && (component == 0 || i == len - 1))
				return 0;

		/*
		 * '.' must not be at the end. It may be first overall
		 * but must not otherwise start a component.
		 */
		if (c == '.' && ((component == 0 && !first) || i == len - 1))
				return 0;

		if (c == '.') {
			/* Components can not end with a dash. */
			if (prev == '-')
				return 0;
			/* Start new component */
			component = 0;
			continue;
		}
		/* Components must be 63 chars or less. */
		if (++component > 63)
			return 0;
	}
	return 1;
}

int
x509_constraints_valid_domain(uint8_t *name, size_t len)
{
	if (len == 0)
		return 0;
	if (name[0] == '*') /* wildcard not allowed in a domain name */
		return 0;
	/*
	 * A domain may not be less than two characters, so you can't
	 * have a require subdomain name with less than that.
	 */
	if (len < 3 && name[0] == '.')
		return 0;
	return x509_constraints_valid_domain_internal(name, len);
}

int
x509_constraints_valid_host(uint8_t *name, size_t len)
{
	struct sockaddr_in sin4;
	struct sockaddr_in6 sin6;

	if (len == 0)
		return 0;
	if (name[0] == '*') /* wildcard not allowed in a host name */
		return 0;
	if (name[0] == '.') /* leading . not allowed in a host name*/
		return 0;
	if (inet_pton(AF_INET, name, &sin4) == 1)
		return 0;
	if (inet_pton(AF_INET6, name, &sin6) == 1)
		return 0;
	return x509_constraints_valid_domain_internal(name, len);
}

int
x509_constraints_valid_sandns(uint8_t *name, size_t len)
{
	if (len == 0)
		return 0;

	if (name[0] == '.') /* leading . not allowed in a SAN DNS name */
		return 0;
	/*
	 * A domain may not be less than two characters, so you
	 * can't wildcard a single domain of less than that
	 */
	if (len < 4 && name[0] == '*')
		return 0;
	/*
	 * A wildcard may only be followed by a '.'
	 */
	if (len >= 4 && name[0] == '*' && name[1] != '.')
		return 0;

	return x509_constraints_valid_domain_internal(name, len);
}

static inline int
local_part_ok(char c)
{
	return (('0' <= c && c <= '9') || ('a' <= c && c <= 'z') ||
	    ('A' <= c && c <= 'Z') || c == '!' || c == '#' || c == '$' ||
	    c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
	    c == '-' || c == '/' || c == '=' || c == '?' ||  c == '^' ||
	    c == '_' || c == '`' || c == '{' || c == '|' || c == '}' ||
	    c == '~' || c == '.');
}

/*
 * Parse "candidate" as an RFC 2821 mailbox.
 * Returns 0 if candidate is not a valid mailbox or if an error occurs.
 * Returns 1 if candidate is a mailbox and adds newly allocated
 * local and domain parts of the mailbox to "name->local" and name->name"
 */
int
x509_constraints_parse_mailbox(uint8_t *candidate, size_t len,
    struct x509_constraints_name *name)
{
	char working[DOMAIN_PART_MAX_LEN + 1] = { 0 };
	char *candidate_local = NULL;
	char *candidate_domain = NULL;
	size_t i, wi = 0;
	int accept = 0;
	int quoted = 0;

	if (candidate == NULL)
		return 0;

	/* It can't be bigger than the local part, domain part and the '@' */
	if (len > LOCAL_PART_MAX_LEN + DOMAIN_PART_MAX_LEN + 1)
		return 0;

	for (i = 0; i < len; i++) {
		char c = candidate[i];
		/* non ascii, cr, lf, or nul is never allowed */
		if (!isascii(c) || c == '\r' || c == '\n' || c == '\0')
			goto bad;
		if (i == 0) {
			/* local part is quoted part */
			if (c == '"')
				quoted = 1;
			/* can not start with a . */
			if (c == '.')
				goto bad;
		}
		if (wi > DOMAIN_PART_MAX_LEN)
			goto bad;
		if (accept) {
			working[wi++] = c;
			accept = 0;
			continue;
		}
		if (candidate_local != NULL) {
			/* We are looking for the domain part */
			if (wi > DOMAIN_PART_MAX_LEN)
				goto bad;
			working[wi++] = c;
			if (i == len - 1) {
				if (wi == 0)
					goto bad;
				if (candidate_domain != NULL)
					goto bad;
				candidate_domain = strdup(working);
				if (candidate_domain == NULL)
					goto bad;
			}
			continue;
		}
		/* We are looking for the local part */
		if (wi > LOCAL_PART_MAX_LEN)
			break;

		if (quoted) {
			if (c == '\\') {
				accept = 1;
				continue;
			}
			if (c == '"' && i != 0) {
				/* end the quoted part. @ must be next */
				if (i + 1 == len || candidate[i + 1] != '@')
					goto bad;
				quoted = 0;
			}
			/*
			 * XXX Go strangely permits sp but forbids ht
			 * mimic that for now
			 */
			if (c == 9)
				goto bad;
			working[wi++] = c;
			continue; /* all's good inside our quoted string */
		}
		if (c == '@') {
			if (wi == 0)
				goto bad;;
			if (candidate_local != NULL)
				goto bad;
			candidate_local = strdup(working);
			if (candidate_local == NULL)
				goto bad;
			memset(working, 0, sizeof(working));
			wi = 0;
			continue;
		}
		if (c == '\\') {
			/*
			 * RFC 3936 hints these can happen outside of
			 * quotend string. don't include the \ but
			 * next character must be ok.
			 */
			if (i + 1 == len)
				goto bad;
			if (!local_part_ok(candidate[i + 1]))
				goto bad;
			accept = 1;
		}
		if (!local_part_ok(c))
			goto bad;
		working[wi++] = c;
	}
	if (candidate_local == NULL || candidate_domain == NULL)
		goto bad;
	if (!x509_constraints_valid_host(candidate_domain,
	    strlen(candidate_domain)))
		goto bad;

	name->local = candidate_local;
	name->name = candidate_domain;
	name->type = GEN_EMAIL;
	return 1;
 bad:
	free(candidate_local);
	free(candidate_domain);
	return 0;
}

int
x509_constraints_valid_domain_constraint(uint8_t *constraint, size_t len)
{
	if (len == 0)
		return 1; 	/* empty constraints match */

	if (constraint[0] == '*') /* wildcard not allowed in a constraint */
		return 0;

	/*
	 * A domain may not be less than two characters, so you
	 * can't match a single domain of less than that
	 */
	if (len < 3 && constraint[0] == '.')
		return 0;
	return x509_constraints_valid_domain_internal(constraint, len);
}

/*
 * Extract the host part of a URI, returns the host part as a c string
 * the caller must free, or or NULL if it could not be found or is
 * invalid.
 *
 * RFC 3986:
 * the authority part of a uri starts with // and is terminated with
 * the next '/', '?', '#' or end of the URI.
 *
 * The authority itself contains [userinfo '@'] host [: port]
 *
 * so the host starts at the start or after the '@', and ends
 * with end of URI, '/', '?', "#', or ':'.
 */
int
x509_constraints_uri_host(uint8_t *uri, size_t len, char **hostpart)
{
	size_t i, hostlen = 0;
	uint8_t *authority = NULL;
	char *host = NULL;

	/*
	 * Find first '//'. there must be at least a '//' and
	 * something else.
	 */
	if (len < 3)
		return 0;
	for (i = 0; i < len - 1; i++) {
		if (!isascii(uri[i]))
			return 0;
		if (uri[i] == '/' && uri[i + 1] == '/') {
			authority = uri + i + 2;
			break;
		}
	}
	if (authority == NULL)
		return 0;
	for (i = authority - uri; i < len; i++) {
		if (!isascii(uri[i]))
			return 0;
		/* it has a userinfo part */
		if (uri[i] == '@') {
			hostlen = 0;
			/* it can only have one */
			if (host != NULL)
				break;
			/* start after the userinfo part */
			host = uri + i + 1;
			continue;
		}
		/* did we find the end? */
		if (uri[i] == ':' || uri[i] == '/' || uri[i] == '?' ||
		    uri[i] == '#')
			break;
		hostlen++;
	}
	if (hostlen == 0)
		return 0;
	if (host == NULL)
		host = authority;
	if (!x509_constraints_valid_host(host, hostlen))
		return 0;
	*hostpart = strndup(host, hostlen);
        return 1;
}

int
x509_constraints_sandns(char *sandns, size_t dlen, char *constraint,
    size_t len)
{
	char *suffix;

	if (len == 0)
		return 1; /* an empty constraint matches everything */

	/* match the end of the domain */
	if (dlen < len)
		return 0;
	suffix = sandns + (dlen - len);
	return (strncasecmp(suffix, constraint, len) == 0);
}

/*
 * Validate a pre-validated domain of length dlen against a pre-validated
 * constraint of length len.
 *
 * returns 1 if the domain and constraint match.
 * returns 0 otherwise.
 *
 * an empty constraint matches everyting.
 * constraint will be matched against the domain as a suffix if it
 * starts with a '.'.
 * domain will be matched against the constraint as a suffix if it
 * starts with a '.'.
 */
int
x509_constraints_domain(char *domain, size_t dlen, char *constraint,
    size_t len)
{
	if (len == 0)
		return 1; /* an empty constraint matches everything */

	if (constraint[0] == '.') {
		/* match the end of the domain */
		char *suffix;
		if (dlen < len)
			return 0;
		suffix = domain + (dlen - len);
		return (strncasecmp(suffix, constraint, len) == 0);
	}
	if (domain[0] == '.') {
		/* match the end of the constraint */
		char *suffix;
		if (len < dlen)
			return 0;
		suffix = constraint + (len - dlen);
		return (strncasecmp(suffix, domain, dlen) == 0);
	}
	/* otherwise we must exactly match the constraint */
	if (dlen != len)
		return 0;
	return (strncasecmp(domain, constraint, len) == 0);
}

int
x509_constraints_uri(uint8_t *uri, size_t ulen, uint8_t *constraint,
    size_t len, int *error)
{
	int ret = 0;
	char *hostpart = NULL;

	if (!x509_constraints_uri_host(uri, ulen, &hostpart)) {
		*error = X509_V_ERR_UNSUPPORTED_NAME_SYNTAX;
		goto err;
	}
	if (hostpart == NULL) {
		*error = X509_V_ERR_OUT_OF_MEM;
		goto err;
	}
	if (!x509_constraints_valid_domain_constraint(constraint, len)) {
		*error = X509_V_ERR_UNSUPPORTED_CONSTRAINT_SYNTAX;
		goto err;
	}
	ret = x509_constraints_domain(hostpart, strlen(hostpart),
	    constraint, len);
 err:
	free(hostpart);
	return ret;
}

/*
 * Verify a validated address of size alen with a validated contraint
 * of size constraint_len. returns 1 if matching, 0 if not.
 * Addresses are assumed to be pre-validated for a length of 4 and 8
 * respectively for ipv4 addreses and constraints, and a length of
 * 16 and 32 respectively for ipv6 address constraints by the caller.
 */
int
x509_constraints_ipaddr(uint8_t *address, size_t alen,
    uint8_t *constraint, size_t len)
{
	uint8_t *mask;
	size_t i;

	if (alen * 2 != len)
		return 0;

	mask = constraint + alen;
	for (i = 0; i < alen; i++) {
		if ((address[i] & mask[i]) != (constraint[i] & mask[i]))
			return 0;
	}
	return 1;
}

/*
 * Verify a canonicalized der encoded constraint dirname
 * a canonicalized der encoded constraint.
 */
int
x509_constraints_dirname(uint8_t *dirname, size_t dlen,
    uint8_t *constraint, size_t len)
{
	if (len != dlen)
		return 0;
	return (memcmp(constraint, dirname, len) == 0);
}

/*
 * De-obfuscate a GENERAL_NAME into useful bytes for a name or constraint.
 */
int
x509_constraints_general_to_bytes(GENERAL_NAME *name, uint8_t **bytes,
    size_t *len)
{
	*bytes = NULL;
	*len = 0;

	if (name->type == GEN_DNS) {
		ASN1_IA5STRING *aname = name->d.dNSName;
		*bytes = aname->data;
		*len = strlen(aname->data);
		return name->type;
	}
	if (name->type == GEN_EMAIL) {
		ASN1_IA5STRING *aname = name->d.rfc822Name;
		*bytes = aname->data;
		*len = strlen(aname->data);
		return name->type;
	}
	if (name->type == GEN_URI) {
		ASN1_IA5STRING *aname = name->d.uniformResourceIdentifier;
		*bytes = aname->data;
		*len = strlen(aname->data);
		return name->type;
	}
	if (name->type == GEN_DIRNAME) {
		X509_NAME *dname = name->d.directoryName;
		if (!dname->modified || i2d_X509_NAME(dname, NULL) >= 0) {
			*bytes = dname->canon_enc;
			*len = dname->canon_enclen;
			return name->type;
		}
	}
	if (name->type == GEN_IPADD) {
		*bytes = name->d.ip->data;
		*len = name->d.ip->length;
		return name->type;
	}
	return 0;
}


/*
 * Extract the relevant names for constraint checking from "cert",
 * validate them, and add them to the list of cert names for "chain".
 * returns 1 on success sets error and returns 0 on failure.
 */
int
x509_constraints_extract_names(struct x509_constraints_names *names,
    X509 *cert, int is_leaf, int *error)
{
	struct x509_constraints_name *vname = NULL;
	X509_NAME *subject_name;
	GENERAL_NAME *name;
	ssize_t i = 0;
	int name_type, include_cn = is_leaf, include_email = is_leaf;

	/* first grab the altnames */
	while ((name = sk_GENERAL_NAME_value(cert->altname, i++)) != NULL) {
		uint8_t *bytes = NULL;
		size_t len = 0;

		if ((vname = x509_constraints_name_new()) == NULL) {
			*error = X509_V_ERR_OUT_OF_MEM;
			goto err;
		}

		name_type = x509_constraints_general_to_bytes(name, &bytes,
		    &len);
		switch(name_type) {
		case GEN_DNS:
			if (!x509_constraints_valid_sandns(bytes, len)) {
				*error =
				    X509_V_ERR_UNSUPPORTED_NAME_SYNTAX;
				goto err;
			}
			if ((vname->name = strdup(bytes)) == NULL) {
				*error = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			vname->type=GEN_DNS;
			include_cn = 0; /* don't use cn from subject */
			break;
		case GEN_EMAIL:
			if (!x509_constraints_parse_mailbox(bytes, len,
			    vname)) {
				*error = X509_V_ERR_UNSUPPORTED_NAME_SYNTAX;
				goto err;
			}
			vname->type = GEN_EMAIL;
			include_email = 0; /* don't use email from subject */
			break;
		case GEN_URI:
			if (!x509_constraints_uri_host(bytes, len, &vname->name)) {
				*error = X509_V_ERR_UNSUPPORTED_NAME_SYNTAX;
				goto err;
			}
			if (vname->name == NULL) {
				*error = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			vname->type = GEN_URI;
			break;
		case GEN_DIRNAME:
			if (bytes == NULL || ((vname->der = malloc(len)) ==
			    NULL)) {
				*error = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			if (len == 0) {
				*error = X509_V_ERR_UNSUPPORTED_NAME_SYNTAX;
				goto err;
			}
			memcpy(vname->der, bytes, len);
			vname->der_len = len;
			vname->type = GEN_DIRNAME;
			break;
		case GEN_IPADD:
			if (len == 4)
				vname->af = AF_INET;
			if (len == 16)
				vname->af = AF_INET6;
			if (vname->af != AF_INET && vname->af !=
			    AF_INET6) {
				*error =
				    X509_V_ERR_UNSUPPORTED_NAME_SYNTAX;
				goto err;
			}
			memcpy(vname->address, bytes, len);
			vname->type = GEN_IPADD;
			break;
		default:
			/* Ignore this name */
			x509_constraints_name_free(vname);
			vname = NULL;
			continue;
		}
		if (!x509_constraints_names_add(names, vname)) {
			*error = X509_V_ERR_OUT_OF_MEM;
			goto err;
		}
		vname = NULL;
	}
	subject_name = X509_get_subject_name(cert);
	if (X509_NAME_entry_count(subject_name) > 0) {
		struct x509_constraints_name *vname = NULL;
		X509_NAME_ENTRY *email;
		X509_NAME_ENTRY *cn;
		/*
		 * This cert has a non-empty subject, so we must add
		 * the subject as a dirname to be compared against
		 * any dirname constraints
		 */
		if ((subject_name->modified &&
		    i2d_X509_NAME(subject_name, NULL) < 0) ||
		    (vname = x509_constraints_name_new()) == NULL ||
		    (vname->der = malloc(subject_name->canon_enclen)) == NULL) {
			*error = X509_V_ERR_OUT_OF_MEM;
			goto err;
		}

		memcpy(vname->der, subject_name->canon_enc,
		    subject_name->canon_enclen);
		vname->der_len = subject_name->canon_enclen;
		vname->type = GEN_DIRNAME;
		if (!x509_constraints_names_add(names, vname)) {
			*error = X509_V_ERR_OUT_OF_MEM;
			goto err;
		}
		vname = NULL;
		/*
		 * Get any email addresses from the subject line, and
		 * add them as mbox names to be compared against any
		 * email constraints
		 */
		while (include_email &&
		    (i = X509_NAME_get_index_by_NID(subject_name,
		    NID_pkcs9_emailAddress, i)) >= 0) {
			ASN1_STRING *aname;
			if ((email = X509_NAME_get_entry(subject_name, i)) == NULL ||
			    (aname = X509_NAME_ENTRY_get_data(email)) == NULL) {
				*error = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			if ((vname = x509_constraints_name_new()) == NULL) {
				*error = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			if (!x509_constraints_parse_mailbox(aname->data,
			    aname->length, vname)) {
				*error = X509_V_ERR_UNSUPPORTED_NAME_SYNTAX;
				goto err;
			}
			vname->type = GEN_EMAIL;
			if (!x509_constraints_names_add(names, vname)) {
				*error = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			vname = NULL;
		}
		/*
		 * Include the CN as a hostname to be checked againt
		 * name constraints if it looks like a hostname.
		 */
		while (include_cn &&
		    (i = X509_NAME_get_index_by_NID(subject_name,
		    NID_commonName, i)) >= 0) {
			ASN1_STRING *aname;
			if ((cn = X509_NAME_get_entry(subject_name, i)) == NULL ||
			    (aname = X509_NAME_ENTRY_get_data(cn)) == NULL) {
				*error = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			if (!x509_constraints_valid_host(aname->data,
			    aname->length))
				continue; /* ignore it if not a hostname */
			if ((vname = x509_constraints_name_new()) == NULL) {
				*error = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			if ((vname->name = strndup(aname->data,
			    aname->length)) == NULL) {
				*error = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			vname->type = GEN_DNS;
			if (!x509_constraints_names_add(names, vname)) {
				*error = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			vname = NULL;
		}
	}
	return 1;
 err:
	x509_constraints_name_free(vname);
	return 0;
}

/*
 * Validate a constraint in a general name, putting the relevant data
 * into "name" if valid. returns 0, and sets error if the constraint is
 * not valid. returns 1 if the constraint validated. name->type will be
 * set to a valid type if there is constraint data in name, or unmodified
 * if the GENERAL_NAME had a valid type but was ignored.
 */
int
x509_constraints_validate(GENERAL_NAME *constraint,
    struct x509_constraints_name *name, int *error)
{
	uint8_t *bytes = NULL;
	size_t len = 0;
	int name_type;

	name_type = x509_constraints_general_to_bytes(constraint, &bytes, &len);
	switch (name_type) {
	case GEN_DIRNAME:
		if (bytes == NULL || (name->der = malloc(len)) == NULL) {
			*error = X509_V_ERR_OUT_OF_MEM;
			return 0;
		}
		if (len == 0)
			goto err; /* XXX The RFC's are delightfully vague */
		memcpy(name->der, bytes, len);
		name->der_len = len;
		name->type = GEN_DIRNAME;
		break;
	case GEN_DNS:
		if (!x509_constraints_valid_domain_constraint(bytes,
		    len))
			goto err;
		if ((name->name = strdup(bytes)) == NULL) {
			*error = X509_V_ERR_OUT_OF_MEM;
			return 0;
		}
		name->type = GEN_DNS;
		break;
	case GEN_EMAIL:
		if (memchr(bytes, '@', len) != NULL) {
			if (!x509_constraints_parse_mailbox(bytes, len,
			    name))
				goto err;
		} else {
			if (!x509_constraints_valid_domain_constraint(
			    bytes, len))
				goto err;
			if ((name->name = strdup(bytes)) == NULL) {
				*error = X509_V_ERR_OUT_OF_MEM;
				return 0;
			}
		}
		name->type = GEN_EMAIL;
		break;
	case GEN_IPADD:
		/* Constraints are ip then mask */
		if (len == 8)
			name->af = AF_INET;
		else if (len == 32)
			name->af = AF_INET6;
		else
			goto err;
		memcpy(&name->address[0], bytes, len);
		name->type = GEN_IPADD;
		break;
	case GEN_URI:
		if (!x509_constraints_valid_domain_constraint(bytes,
		    len))
			goto err;
		name->name = strdup(bytes);
		name->type = GEN_URI;
		break;
	default:
		break;
	}
	return 1;
 err:
	*error = X509_V_ERR_UNSUPPORTED_CONSTRAINT_SYNTAX;
	return 0;
}

int
x509_constraints_extract_constraints(X509 *cert,
    struct x509_constraints_names *permitted,
    struct x509_constraints_names *excluded,
    int *error)
{
	struct x509_constraints_name *vname;
	NAME_CONSTRAINTS *nc = cert->nc;
	GENERAL_SUBTREE *subtree;
	int i;

	if (nc == NULL)
		return 1;

	for (i = 0; i < sk_GENERAL_SUBTREE_num(nc->permittedSubtrees); i++) {

		subtree = sk_GENERAL_SUBTREE_value(nc->permittedSubtrees, i);
		if (subtree->minimum || subtree->maximum) {
			*error = X509_V_ERR_SUBTREE_MINMAX;
			return 0;
		}
		if ((vname = x509_constraints_name_new()) == NULL) {
			*error = X509_V_ERR_OUT_OF_MEM;
			return 0;
		}
		if (x509_constraints_validate(subtree->base, vname, error) ==
		    0) {
			x509_constraints_name_free(vname);
			return 0;
		}
		if (vname->type == 0) {
			x509_constraints_name_free(vname);
			continue;
		}
		if (!x509_constraints_names_add(permitted, vname)) {
			x509_constraints_name_free(vname);
 			*error = X509_V_ERR_OUT_OF_MEM;
			return 0;
		}
	}

	for (i = 0; i < sk_GENERAL_SUBTREE_num(nc->excludedSubtrees); i++) {
		subtree = sk_GENERAL_SUBTREE_value(nc->excludedSubtrees, i);
		if (subtree->minimum || subtree->maximum) {
			*error = X509_V_ERR_SUBTREE_MINMAX;
			return 0;
		}
		if ((vname = x509_constraints_name_new()) == NULL) {
			*error = X509_V_ERR_OUT_OF_MEM;
			return 0;
		}
		if (x509_constraints_validate(subtree->base, vname, error) ==
		    0) {
			x509_constraints_name_free(vname);
			return 0;
		}
		if (vname->type == 0) {
			x509_constraints_name_free(vname);
			continue;
		}
		if (!x509_constraints_names_add(excluded, vname)) {
			x509_constraints_name_free(vname);
 			*error = X509_V_ERR_OUT_OF_MEM;
			return 0;
		}
	}

	return 1;
}

/*
 * Match a validated name in "name" against a validated constraint in
 * "constraint" return 1 if then name matches, 0 otherwise.
 */
int
x509_constraints_match(struct x509_constraints_name *name,
    struct x509_constraints_name *constraint)
{
	if (name->type != constraint->type)
		return 0;
	if (name->type == GEN_DNS)
		return x509_constraints_sandns(name->name,
		    strlen(name->name), constraint->name,
		    strlen(constraint->name));
	if (name->type == GEN_URI)
		return x509_constraints_domain(name->name,
		    strlen(name->name), constraint->name,
		    strlen(constraint->name));
	if (name->type == GEN_IPADD) {
		size_t nlen = name->af == AF_INET ? 4 : 16;
		size_t clen = name->af == AF_INET ? 8 : 32;
		if (name->af != AF_INET && name->af != AF_INET6)
			return 0;
		if (constraint->af != AF_INET && constraint->af != AF_INET6)
			return 0;
		if (name->af != constraint->af)
			return 0;
		return x509_constraints_ipaddr(name->address,
		    nlen, constraint->address, clen);
	}
	if (name->type == GEN_EMAIL) {
		if (constraint->local) {
			/* mailbox local and domain parts must exactly match */
			return (strcmp(name->local, constraint->local) == 0 &&
			    strcmp(name->name, constraint->name) == 0);
		}
		/* otherwise match the constraint to the domain part */
		return x509_constraints_domain(name->name,
		    strlen(name->name), constraint->name,
		    strlen(constraint->name));
	}
	if (name->type == GEN_DIRNAME)
		return x509_constraints_dirname(name->der, name->der_len,
		    constraint->der, constraint->der_len);
	return 0;
}

/*
 * Make sure every name in names does not match any excluded
 * constraints, and does match at least one permitted constraint if
 * any are present. Returns 1 if ok, 0, and sets error if not.
 */
int
x509_constraints_check(struct x509_constraints_names *names,
    struct x509_constraints_names *permitted,
    struct x509_constraints_names *excluded, int *error)
{
	size_t i, j;

	for (i = 0; i < names->names_count; i++) {
		int permitted_seen = 0;
		int permitted_matched = 0;

		for (j = 0; j < excluded->names_count; j++) {
			if (x509_constraints_match(names->names[i],
			    excluded->names[j])) {
				*error = X509_V_ERR_EXCLUDED_VIOLATION;
				return 0;
			}
		}
		for (j = 0; j < permitted->names_count; j++) {
			if (permitted->names[j]->type == names->names[i]->type)
				permitted_seen++;
			if (x509_constraints_match(names->names[i],
			    permitted->names[j])) {
				permitted_matched++;
				break;
			}
		}
		if (permitted_seen && !permitted_matched) {
			*error = X509_V_ERR_PERMITTED_VIOLATION;
			return 0;
		}
	}
	return 1;
}

/*
 * Walk a validated chain of X509 certs, starting at the leaf, and
 * validate the name constraints in the chain. Intended for use with
 * the legacy X509 validtion code in x509_vfy.c
 *
 * returns 1 if the constraints are ok, 0 otherwise, setting error and
 * depth
 */
int
x509_constraints_chain(STACK_OF(X509) *chain, int *error, int *depth)
{
	int chain_length, verify_err = X509_V_ERR_UNSPECIFIED, i = 0;
	struct x509_constraints_names *names = NULL;
	struct x509_constraints_names *excluded = NULL;
	struct x509_constraints_names *permitted = NULL;
	size_t constraints_count = 0;
	X509 *cert;

	if (chain == NULL || (chain_length = sk_X509_num(chain)) == 0)
		goto err;
	if (chain_length == 1)
		return 1;
	if ((names = x509_constraints_names_new()) == NULL) {
		verify_err = X509_V_ERR_OUT_OF_MEM;
		goto err;
	}

	if ((cert = sk_X509_value(chain, 0)) == NULL)
		goto err;
	if (!x509_constraints_extract_names(names, cert, 1, &verify_err))
		goto err;
	for (i = 1; i < chain_length; i++) {
		if ((cert = sk_X509_value(chain, i)) == NULL)
			goto err;
		if (cert->nc != NULL) {
			if ((permitted =
			    x509_constraints_names_new()) == NULL) {
				verify_err = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			if ((excluded =
			    x509_constraints_names_new()) == NULL) {
				verify_err = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			if (!x509_constraints_extract_constraints(cert,
			    permitted, excluded, &verify_err))
				goto err;
			constraints_count += permitted->names_count;
			constraints_count += excluded->names_count;
			if (constraints_count >
			    X509_VERIFY_MAX_CHAIN_CONSTRAINTS) {
				verify_err = X509_V_ERR_OUT_OF_MEM;
				goto err;
			}
			if (!x509_constraints_check(names, permitted,
			    excluded, &verify_err))
				goto err;
			x509_constraints_names_free(excluded);
			excluded = NULL;
			x509_constraints_names_free(permitted);
			permitted = NULL;
		}
		if (!x509_constraints_extract_names(names, cert, 0,
		    &verify_err))
			goto err;
		if (names->names_count > X509_VERIFY_MAX_CHAIN_NAMES) {
			verify_err = X509_V_ERR_OUT_OF_MEM;
			goto err;
		}
	}

	x509_constraints_names_free(names);
	return 1;

 err:
	*error = verify_err;
	*depth = i;
	x509_constraints_names_free(excluded);
	x509_constraints_names_free(permitted);
	x509_constraints_names_free(names);
	return 0;
}
