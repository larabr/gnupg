/* protect.c - Un/Protect a secret key
 * Copyright (C) 1998, 1999, 2000, 2001, 2002 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

#include "agent.h"

#define PROT_CIPHER        GCRY_CIPHER_AES
#define PROT_CIPHER_STRING "aes"
#define PROT_CIPHER_KEYLEN (128/8)


/* A table containing the information needed to create a protected
   private key */
static struct {
  const char *algo;
  const char *parmlist;
  int prot_from, prot_to;
} protect_info[] = {
  { "rsa",  "nedpqu", 2, 5 },
  { NULL }
};


static int
hash_passphrase (const char *passphrase, int hashalgo,
                 int s2kmode,
                 const unsigned char *s2ksalt, unsigned long s2kcount,
                 unsigned char *key, size_t keylen);



/* Return the length of the next S-Exp part and update the pointer to
   the first data byte.  0 is return on error */
static size_t
snext (unsigned char const **buf)
{
  const unsigned char *s;
  int n;

  s = *buf;
  for (n=0; *s && *s != ':' && digitp (s); s++)
    n = n*10 + atoi_1 (s);
  if (!n || *s != ':')
    return 0; /* we don't allow empty lengths */
  *buf = s+1;
  return n;
}

/* Skip over the S-Expression BUF points to and update BUF to point to
   the chacter right behind.  DEPTH gives the initial number of open
   lists and may be passed as a positive number to skip over the
   remainder of an S-Expression if the current position is somewhere
   in an S-Expression.  The function may return an error code if it
   encounters an impossible conditions */
static int
sskip (unsigned char const **buf, int *depth)
{
  const unsigned char *s = *buf;
  size_t n;
  int d = *depth;
  
  while (d > 0)
    {
      if (*s == '(')
        {
          d++;
          s++;
        }
      else if (*s == ')')
        {
          d--;
          s++;
        }
      else
        {
          if (!d)
            return GNUPG_Invalid_Sexp;
          n = snext (&s);
          if (!n)
            return GNUPG_Invalid_Sexp; 
          s += n;
        }
    }
  *buf = s;
  *depth = d;
  return 0;
}


/* Check whether the the string at the address BUF points to matches
   the token.  Return true on match and update BUF to point behind the
   token. */
static int
smatch (unsigned char const **buf, size_t buflen, const char *token)
{
  size_t toklen = strlen (token);

  if (buflen != toklen || memcmp (*buf, token, toklen))
    return 0;
  *buf += toklen;
  return 1;
}



/* Calculate the MIC for a private key S-Exp. SHA1HASH should pint to
   a 20 byte buffer.  This function is suitable for any algorithms. */
static int 
calculate_mic (const unsigned char *plainkey, unsigned char *sha1hash)
{
  const unsigned char *hash_begin, *hash_end;
  const unsigned char *s;
  size_t n;

  s = plainkey;
  if (*s != '(')
    return GNUPG_Invalid_Sexp;
  s++;
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 
  if (!smatch (&s, n, "private-key"))
    return GNUPG_Unknown_Sexp; 
  if (*s != '(')
    return GNUPG_Unknown_Sexp;
  hash_begin = s;
  s++;
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 
  s += n; /* skip over the algorithm name */

  while (*s == '(')
    {
      s++;
      n = snext (&s);
      if (!n)
        return GNUPG_Invalid_Sexp; 
      s += n;
      n = snext (&s);
      if (!n)
        return GNUPG_Invalid_Sexp; 
      s += n;
      if ( *s != ')' )
        return GNUPG_Invalid_Sexp; 
      s++;
    }
  if (*s != ')')
    return GNUPG_Invalid_Sexp; 
  s++;
  hash_end = s;

  gcry_md_hash_buffer (GCRY_MD_SHA1, sha1hash,
                       hash_begin, hash_end - hash_begin);

  return 0;
}



/* Encrypt the parameter block starting at PROTBEGIN with length
   PROTLEN using the utf8 encoded key PASSPHRASE and return the entire
   encrypted block in RESULT or ereturn with an error code.  SHA1HASH
   is the 20 byte SHA-1 hash required for the integrity code.

   The parameter block is expected to be an incomplete S-Expression of
   the form (example in advanced format):

     (d #046129F..[some bytes not shown]..81#)
     (p #00e861b..[some bytes not shown]..f1#)
     (q #00f7a7c..[some bytes not shown]..61#)
     (u #304559a..[some bytes not shown]..9b#) 

   the returned block is the S-Expression:

    (protected mode (parms) encrypted_octet_string)

*/
static int
do_encryption (const char *protbegin, size_t protlen, 
               const char *passphrase,  const unsigned char *sha1hash,
               unsigned char **result, size_t *resultlen)
{
  GCRY_CIPHER_HD hd;
  const char *modestr = "openpgp-s2k3-sha1-" PROT_CIPHER_STRING "-cbc";
  int blklen, enclen, outlen;
  char *iv = NULL;
  int rc = 0;
  char *outbuf = NULL;
  char *p;
  int saltpos, ivpos, encpos;

  hd = gcry_cipher_open (PROT_CIPHER, GCRY_CIPHER_MODE_CBC,
                         GCRY_CIPHER_SECURE);
  if (!hd)
    return map_gcry_err (gcry_errno());


  /* We need to work on a copy of the data because this makes it
     easier to add the trailer and the padding and more important we
     have to prefix the text with 2 parenthesis, so we have to
     allocate enough space for:

     ((<parameter_list>)(4:hash4:sha120:<hashvalue>)) + padding

     We always append a full block of random bytes as padding but
     encrypt only what is needed for a full blocksize */
  blklen = gcry_cipher_get_algo_blklen (PROT_CIPHER);
  outlen = 2 + protlen + 2 + 6 + 6 + 23 + 2 + blklen;
  enclen = outlen/blklen * blklen;
  outbuf = gcry_malloc_secure (outlen);
  if (!outbuf)
    rc = GNUPG_Out_Of_Core;
  if (!rc)
    {
      /* allocate random bytes to be used as IV, padding and s2k salt*/
      iv = gcry_random_bytes (blklen*2+8, GCRY_WEAK_RANDOM);
      if (!iv)
        rc = GNUPG_Out_Of_Core;
      else
        rc = gcry_cipher_setiv (hd, iv, blklen);
    }
  if (!rc)
    {
      unsigned char *key;
      size_t keylen = PROT_CIPHER_KEYLEN;
      
      key = gcry_malloc_secure (keylen);
      if (!key)
        rc = GNUPG_Out_Of_Core;
      else
        {
          rc = hash_passphrase (passphrase, GCRY_MD_SHA1,
                                3, iv+2*blklen, 96, key, keylen);
          if (!rc)
            rc = gcry_cipher_setkey (hd, key, keylen);
          xfree (key);
        }
    }
  if (!rc)
    {
      p = outbuf;
      *p++ = '(';
      *p++ = '(';
      memcpy (p, protbegin, protlen);
      p += protlen;
      memcpy (p, ")(4:hash4:sha120:", 17);
      p += 17;
      memcpy (p, sha1hash, 20);
      p += 20;
      *p++ = ')';
      *p++ = ')';
      memcpy (p, iv+blklen, blklen); 
      p += blklen;
      assert ( p - outbuf == outlen);
      rc = gcry_cipher_encrypt (hd, outbuf, enclen, NULL, 0);
    }
  gcry_cipher_close (hd);
  if (rc)
    {
      xfree (iv);
      xfree (outbuf);
      return rc;
    }

  /* Now allocate the buffer we want to return.  This is

     (protected openpgp-s2k3-sha1-aes-cbc
       ((sha1 salt no_of_iterations) 16byte_iv)
       encrypted_octet_string)
       
     in canoncical format of course.  We use asprintf and %n modifier
     and spaces as palceholders.  */
  asprintf (&p,
            "(9:protected%d:%s((4:sha18:%n_8bytes_2:96)%d:%n%*s)%d:%n%*s)",
            (int)strlen (modestr), modestr,
            &saltpos, 
            blklen, &ivpos, blklen, "",
            enclen, &encpos, enclen, "");
  if (p)
    { /* asprintf does not use out malloc system */
      char *psave = p;
      p = xtrymalloc (strlen (psave)+1);
      if (p)
        strcpy (p, psave);
      free (psave);
    }
  if (!p)
    {
      xfree (iv);
      xfree (outbuf);
      return GNUPG_Out_Of_Core;
    }
  *resultlen = strlen (p);
  *result = p;
  memcpy (p+saltpos, iv+2*blklen, 8);
  memcpy (p+ivpos, iv, blklen);
  memcpy (p+encpos, outbuf, enclen);
  xfree (iv);
  xfree (outbuf);
  return 0;
}



/* Protect the key encoded in canonical format in plainkey.  We assume
   a valid S-Exp here. */
int 
agent_protect (const unsigned char *plainkey, const char *passphrase,
               unsigned char **result, size_t *resultlen)
{
  int rc;
  const unsigned char *s;
  const unsigned char *hash_begin, *hash_end;
  const unsigned char *prot_begin, *prot_end, *real_end;
  size_t n;
  int c, infidx, i;
  unsigned char hashvalue[20];
  unsigned char *protected;
  size_t protectedlen;
  int depth = 0;
  unsigned char *p;

  s = plainkey;
  if (*s != '(')
    return GNUPG_Invalid_Sexp;
  depth++;
  s++;
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 
  if (!smatch (&s, n, "private-key"))
    return GNUPG_Unknown_Sexp; 
  if (*s != '(')
    return GNUPG_Unknown_Sexp;
  depth++;
  hash_begin = s;
  s++;
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 

  for (infidx=0; protect_info[infidx].algo
              && !smatch (&s, n, protect_info[infidx].algo); infidx++)
    ;
  if (!protect_info[infidx].algo)
    return GNUPG_Unsupported_Algorithm; 

  prot_begin = prot_end = NULL;
  for (i=0; (c=protect_info[infidx].parmlist[i]); i++)
    {
      if (i == protect_info[infidx].prot_from)
        prot_begin = s;
      if (*s != '(')
        return GNUPG_Invalid_Sexp;
      depth++;
      s++;
      n = snext (&s);
      if (!n)
        return GNUPG_Invalid_Sexp; 
      if (n != 1 || c != *s)
        return GNUPG_Invalid_Sexp; 
      s += n;
      n = snext (&s);
      if (!n)
        return GNUPG_Invalid_Sexp; 
      s +=n; /* skip value */
      if (*s != ')')
        return GNUPG_Invalid_Sexp; 
      depth--;
      if (i == protect_info[infidx].prot_to)
        prot_end = s;
      s++;
    }
  if (*s != ')' || !prot_begin || !prot_end )
    return GNUPG_Invalid_Sexp; 
  depth--;
  hash_end = s;
  s++;
  /* skip to the end of the S-exp */
  assert (depth == 1);
  rc = sskip (&s, &depth);
  if (rc)
    return rc;
  assert (!depth);
  real_end = s-1;

  gcry_md_hash_buffer (GCRY_MD_SHA1, hashvalue,
                       hash_begin, hash_end - hash_begin + 1);

  rc = do_encryption (prot_begin, prot_end - prot_begin + 1,
                      passphrase,  hashvalue,
                      &protected, &protectedlen);
  if (rc)
    return rc;

  /* Now create the protected version of the key.  Note that the 10
     extra bytes are for for the inserted "protected-" string (the
     beginning of the plaintext reads: "((11:private-key(" ). */
  *resultlen = (10
                + (prot_begin-plainkey)
                + protectedlen
                + (real_end-prot_end));
  *result = p = xtrymalloc (*resultlen);
  if (!p)
    {
      xfree (protected);
      return GNUPG_Out_Of_Core;
    }
  memcpy (p, "(21:protected-", 14);
  p += 14;
  memcpy (p, plainkey+4, prot_begin - plainkey - 4);
  p += prot_begin - plainkey - 4;
  memcpy (p, protected, protectedlen);
  p += protectedlen;
  memcpy (p, prot_end+1, real_end - prot_end);
  p += real_end - prot_end;
  assert ( p - *result == *resultlen);
  xfree (protected);
  return 0;
}


/* Do the actual decryption and check the return list for consistency.  */
static int
do_decryption (const unsigned char *protected, size_t protectedlen, 
               const char *passphrase, 
               const unsigned char *s2ksalt, unsigned long s2kcount,
               const unsigned char *iv, size_t ivlen,
               unsigned char **result)
{
  int rc = 0;
  int blklen;
  GCRY_CIPHER_HD hd;
  unsigned char *outbuf;
  size_t reallen;

  blklen = gcry_cipher_get_algo_blklen (PROT_CIPHER);
  if (protectedlen < 4 || (protectedlen%blklen))
    return GNUPG_Corrupted_Protection;

  hd = gcry_cipher_open (PROT_CIPHER, GCRY_CIPHER_MODE_CBC,
                         GCRY_CIPHER_SECURE);
  if (!hd)
    return map_gcry_err (gcry_errno());

  outbuf = gcry_malloc_secure (protectedlen);
  if (!outbuf)
    rc = GNUPG_Out_Of_Core;
  if (!rc)
    rc = gcry_cipher_setiv (hd, iv, ivlen);
  if (!rc)
    {
      unsigned char *key;
      size_t keylen = PROT_CIPHER_KEYLEN;
      
      key = gcry_malloc_secure (keylen);
      if (!key)
        rc = GNUPG_Out_Of_Core;
      else
        {
          rc = hash_passphrase (passphrase, GCRY_MD_SHA1,
                                3, s2ksalt, s2kcount, key, keylen);
          if (!rc)
            rc = gcry_cipher_setkey (hd, key, keylen);
          xfree (key);
        }
    }
  if (!rc)
    rc = gcry_cipher_decrypt (hd, outbuf, protectedlen,
                              protected, protectedlen);
  gcry_cipher_close (hd);
  if (rc)
    {
      xfree (outbuf);
      return rc;
    }
  /* do a quick check first */
  if (*outbuf != '(' && outbuf[1] != '(')
    {
      xfree (outbuf);
      return GNUPG_Bad_Passphrase;
    }
  /* check that we have a consistent S-Exp */
  reallen = gcry_sexp_canon_len (outbuf, protectedlen, NULL, NULL);
  if (!reallen || (reallen + blklen < protectedlen) )
    {
      xfree (outbuf);
      return GNUPG_Bad_Passphrase;
    }
  *result = outbuf;
  return 0;
}


/* Merge the parameter list contained in CLEARTEXT with the original
   protect lists PROTECTEDKEY by replacing the list at REPLACEPOS.
   Return the new list in RESULT and the MIC value in the 20 byte
   buffer SHA1HASH. */
static int
merge_lists (const unsigned char *protectedkey,
             size_t replacepos, 
             const unsigned char *cleartext,
             unsigned char *sha1hash, unsigned char **result)
{
  size_t n, newlistlen;
  unsigned char *newlist, *p;
  const unsigned char *s;
  const unsigned char *startpos, *endpos;
  int i, rc;
  
  if (replacepos < 26)
    return GNUPG_Bug;

  /* Estimate the required size of the resulting list.  We have a large
     safety margin of >20 bytes (MIC hash from CLEARTEXT and the
     removed "protected-" */
  newlistlen = gcry_sexp_canon_len (protectedkey, 0, NULL, NULL);
  if (!newlistlen)
    return GNUPG_Bug;
  n = gcry_sexp_canon_len (cleartext, 0, NULL, NULL);
  if (!n)
    return GNUPG_Bug;
  newlistlen += n;
  newlist = gcry_malloc_secure (newlistlen);
  if (!newlist)
    return GNUPG_Out_Of_Core;

  /* Copy the initial segment */
  strcpy (newlist, "(11:private-key");
  p = newlist + 15;
  memcpy (p, protectedkey+15+10, replacepos-15-10);
  p += replacepos-15-10;

  /* copy the cleartext */
  s = cleartext;
  if (*s != '(' && s[1] != '(')
    return GNUPG_Bug;  /*we already checked this */
  s += 2;
  startpos = s;
  while ( *s == '(' )
    {
      s++;
      n = snext (&s);
      if (!n)
        goto invalid_sexp;
      s += n;
      n = snext (&s);
      if (!n)
        goto invalid_sexp;
      s += n;
      if ( *s != ')' )
        goto invalid_sexp;
      s++;
    }
  if ( *s != ')' )
    goto invalid_sexp;
  endpos = s;
  s++;
  /* short intermezzo: Get the MIC */
  if (*s != '(')
    goto invalid_sexp;
  s++;
  n = snext (&s);
  if (!smatch (&s, n, "hash"))
    goto invalid_sexp;
  n = snext (&s);
  if (!smatch (&s, n, "sha1"))
    goto invalid_sexp; 
  n = snext (&s);
  if (n != 20)
    goto invalid_sexp;
  memcpy (sha1hash, s, 20);
  s += n;
  if (*s != ')')
    goto invalid_sexp;
  /* end intermezzo */

  /* append the parameter list */
  memcpy (p, startpos, endpos - startpos);
  p += endpos - startpos;
  
  /* skip overt the protected list element in the original list */
  s = protectedkey + replacepos;
  assert (*s == '(');
  s++;
  i = 1;
  rc = sskip (&s, &i);
  if (rc)
    goto failure;
  startpos = s;
  i = 2; /* we are inside this level */
  rc = sskip (&s, &i);
  if (rc)
    goto failure;
  assert (s[-1] == ')');
  endpos = s; /* one behind the end of the list */

  /* append the rest */
  memcpy (p, startpos, endpos - startpos);
  p += endpos - startpos;

  /* ready */
  *result = newlist;
  return 0;

 failure:
  xfree (newlist);
  return rc;

 invalid_sexp:
  xfree (newlist);
  return GNUPG_Invalid_Sexp;
}



/* Unprotect the key encoded in canonical format.  We assume a valid
   S-Exp here. */
int 
agent_unprotect (const unsigned char *protectedkey, const char *passphrase,
                 unsigned char **result, size_t *resultlen)
{
  int rc;
  const unsigned char *s;
  size_t n;
  int infidx, i;
  unsigned char sha1hash[20], sha1hash2[20];
  const unsigned char *s2ksalt;
  unsigned long s2kcount;
  const unsigned char *iv;
  const unsigned char *prot_begin;
  unsigned char *cleartext;
  unsigned char *final;

  s = protectedkey;
  if (*s != '(')
    return GNUPG_Invalid_Sexp;
  s++;
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 
  if (!smatch (&s, n, "protected-private-key"))
    return GNUPG_Unknown_Sexp; 
  if (*s != '(')
    return GNUPG_Unknown_Sexp;
  s++;
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 

  for (infidx=0; protect_info[infidx].algo
              && !smatch (&s, n, protect_info[infidx].algo); infidx++)
    ;
  if (!protect_info[infidx].algo)
    return GNUPG_Unsupported_Algorithm; 

  /* now find the list with the protected information.  Here is an
     example for such a list:
     (protected openpgp-s2k3-sha1-aes-cbc 
        ((sha1 <salt> <count>) <Initialization_Vector>)
        <encrypted_data>)
   */
  for (;;)
    {
      if (*s != '(')
        return GNUPG_Invalid_Sexp;
      prot_begin = s;
      s++;
      n = snext (&s);
      if (!n)
        return GNUPG_Invalid_Sexp; 
      if (smatch (&s, n, "protected"))
        break;
      s += n;
      i = 1;
      rc = sskip (&s, &i);
      if (rc)
        return rc;
    }
  /* found */
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 
  if (!smatch (&s, n, "openpgp-s2k3-sha1-" PROT_CIPHER_STRING "-cbc"))
    return GNUPG_Unsupported_Protection;
  if (*s != '(' || s[1] != '(')
    return GNUPG_Invalid_Sexp;
  s += 2;
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 
  if (!smatch (&s, n, "sha1"))
    return GNUPG_Unsupported_Protection;
  n = snext (&s);
  if (n != 8)
    return GNUPG_Corrupted_Protection;
  s2ksalt = s;
  s += n;
  n = snext (&s);
  if (!n)
    return GNUPG_Corrupted_Protection;
  /* We expect a list close as next, so we can simply use strtoul()
     here.  We might want to check that we only have digits - but this
     is nothing we should worry about */
  if (s[n] != ')' )
    return GNUPG_Invalid_Sexp;
  s2kcount = strtoul (s, NULL, 10);
  if (!s2kcount)
    return GNUPG_Corrupted_Protection;
  s += n;
  s++; /* skip list end */

  n = snext (&s);
  if (n != 16) /* Wrong blocksize for IV (we support ony aes-128) */
    return GNUPG_Corrupted_Protection;
  iv = s;
  s += n;
  if (*s != ')' )
    return GNUPG_Invalid_Sexp;
  s++;
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 
  
  rc = do_decryption (s, n,
                      passphrase, s2ksalt, s2kcount,
                      iv, 16,
                      &cleartext);
  if (rc)
    return rc;

  rc = merge_lists (protectedkey, prot_begin-protectedkey, cleartext,
                    sha1hash, &final);
  xfree (cleartext);
  if (rc)
    return rc;

  rc = calculate_mic (final, sha1hash2);
  if (!rc && memcmp (sha1hash, sha1hash2, 20))
    rc = GNUPG_Corrupted_Protection;
  if (rc)
    {
      xfree (final);
      return rc;
    }

  *result = final;
  *resultlen = gcry_sexp_canon_len (final, 0, NULL, NULL);
  return 0;
}

/* Check the type of the private key, this is one of the constants:
   PRIVATE_KEY_UNKNOWN if we can't figure out the type (this is the
   value 0), PRIVATE_KEY_CLEAR for an unprotected private key.
   PRIVATE_KEY_PROTECTED for an protected private key or
   PRIVATE_KEY_SHADOWED for a sub key where the secret parts are stored
   elsewhere. */
int
agent_private_key_type (const unsigned char *privatekey)
{
  const unsigned char *s;
  size_t n;

  s = privatekey;
  if (*s != '(')
    return PRIVATE_KEY_UNKNOWN;
  s++;
  n = snext (&s);
  if (!n)
    return PRIVATE_KEY_UNKNOWN;
  if (smatch (&s, n, "protected-private-key"))
    return PRIVATE_KEY_PROTECTED;
  if (smatch (&s, n, "shadowed-private-key"))
    return PRIVATE_KEY_SHADOWED;
  if (smatch (&s, n, "private-key"))
    return PRIVATE_KEY_CLEAR;
  return PRIVATE_KEY_UNKNOWN;
}



/* Transform a passphrase into a suitable key of length KEYLEN and
   store this key in the caller provided buffer KEY.  The caller must
   provide an HASHALGO, a valid S2KMODE (see rfc-2440) and depending on
   that mode an S2KSALT of 8 random bytes and an S2KCOUNT (a suitable
   value is 96).
  
   Returns an error code on failure.  */
static int
hash_passphrase (const char *passphrase, int hashalgo,
                 int s2kmode,
                 const unsigned char *s2ksalt,
                 unsigned long s2kcount,
                 unsigned char *key, size_t keylen)
{
  GCRY_MD_HD md;
  int pass, i;
  int used = 0;
  int pwlen = strlen (passphrase);

  if ( (s2kmode != 0 && s2kmode != 1 && s2kmode != 3)
      || !hashalgo || !keylen || !key || !passphrase)
    return GNUPG_Invalid_Value;
  if ((s2kmode == 1 ||s2kmode == 3) && !s2ksalt)
    return GNUPG_Invalid_Value;
  
  md = gcry_md_open (hashalgo, GCRY_MD_FLAG_SECURE);
  if (!md)
    return map_gcry_err (gcry_errno());

  for (pass=0; used < keylen; pass++)
    {
      if (pass)
        {
          gcry_md_reset (md);
          for (i=0; i < pass; i++) /* preset the hash context */
            gcry_md_putc (md, 0);
	}

      if (s2kmode == 1 || s2kmode == 3)
        {
          int len2 = pwlen + 8;
          unsigned long count = len2;

          if (s2kmode == 3)
            {
              count = (16ul + (s2kcount & 15)) << ((s2kcount >> 4) + 6);
              if (count < len2)
                count = len2;
            }

          while (count > len2)
            {
              gcry_md_write (md, s2ksalt, 8);
              gcry_md_write (md, passphrase, pwlen);
              count -= len2;
            }
          if (count < 8)
            gcry_md_write (md, s2ksalt, count);
          else 
            {
              gcry_md_write (md, s2ksalt, 8);
              count -= 8;
              gcry_md_write (md, passphrase, count);
            }
        }
      else
        gcry_md_write (md, passphrase, pwlen);
      
      gcry_md_final (md);
      i = gcry_md_get_algo_dlen (hashalgo);
      if (i > keylen - used)
        i = keylen - used;
      memcpy  (key+used, gcry_md_read (md, hashalgo), i);
      used += i;
    }
  gcry_md_close(md);
  return 0;
}



/* Create a shadow key from a public key.  We use the shadow protocol
  "ti-v1" and insert the S-expressionn SHADOW_INFO.  The resulting
  S-expression is returned in an allocated buffer RESULT will point
  to. The input parameters are expected to be valid canonilized
  S-expressions */
int 
agent_shadow_key (const unsigned char *pubkey,
                  const unsigned char *shadow_info,
                  unsigned char **result)
{
  const unsigned char *s;
  const unsigned char *point;
  size_t n;
  int depth = 0;
  unsigned char *p;
  size_t pubkey_len = gcry_sexp_canon_len (pubkey, 0, NULL,NULL);
  size_t shadow_info_len = gcry_sexp_canon_len (shadow_info, 0, NULL,NULL);

  if (!pubkey_len || !shadow_info_len)
    return GNUPG_Invalid_Value;
  s = pubkey;
  if (*s != '(')
    return GNUPG_Invalid_Sexp;
  depth++;
  s++;
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 
  if (!smatch (&s, n, "public-key"))
    return GNUPG_Unknown_Sexp; 
  if (*s != '(')
    return GNUPG_Unknown_Sexp;
  depth++;
  s++;
  n = snext (&s); 
  if (!n)
    return GNUPG_Invalid_Sexp; 
  s += n; /* skip over the algorithm name */

  while (*s != ')')
    {
      if (*s != '(')
        return GNUPG_Invalid_Sexp;
      depth++;
      s++;
      n = snext (&s);
      if (!n) 
        return GNUPG_Invalid_Sexp; 
      s += n;
      n = snext (&s);
      if (!n)
        return GNUPG_Invalid_Sexp; 
      s +=n; /* skip value */
      if (*s != ')')
        return GNUPG_Invalid_Sexp; 
      depth--;
      s++;
    }
  point = s; /* insert right before the point */
  depth--;
  s++;
  assert (depth == 1);

  /* calculate required length by taking in account: the "shadowed-"
     prefix, the "shadowed", "t1-v1" as well as some parenthesis */
  n = 12 + pubkey_len + 1 + 3+8 + 2+5 + shadow_info_len + 1;
  *result = p = xtrymalloc (n);
  if (!p)
      return GNUPG_Out_Of_Core;
  p = stpcpy (p, "(20:shadowed-private-key");
  /* (10:public-key ...)*/
  memcpy (p, pubkey+14, point - (pubkey+14));
  p += point - (pubkey+14);
  p = stpcpy (p, "(8:shadowed5:t1-v1");
  memcpy (p, shadow_info, shadow_info_len);
  p += shadow_info_len;
  *p++ = ')';
  memcpy (p, point, pubkey_len - (point - pubkey));
  p += pubkey_len - (point - pubkey);

  return 0;
}

/* Parse a canonical encoded shadowed key and return a pointer to the
   inner list with the shadow_info */
int 
agent_get_shadow_info (const unsigned char *shadowkey,
                       unsigned char const **shadow_info)
{
  const unsigned char *s;
  size_t n;
  int depth = 0;

  s = shadowkey;
  if (*s != '(')
    return GNUPG_Invalid_Sexp;
  depth++;
  s++;
  n = snext (&s);
  if (!n)
    return GNUPG_Invalid_Sexp; 
  if (!smatch (&s, n, "shadowed-private-key"))
    return GNUPG_Unknown_Sexp; 
  if (*s != '(')
    return GNUPG_Unknown_Sexp;
  depth++;
  s++;
  n = snext (&s); 
  if (!n)
    return GNUPG_Invalid_Sexp; 
  s += n; /* skip over the algorithm name */

  for (;;)
    {
      if (*s == ')')
        return GNUPG_Unknown_Sexp;
      if (*s != '(')
        return GNUPG_Invalid_Sexp;
      depth++;
      s++;
      n = snext (&s);
      if (!n) 
        return GNUPG_Invalid_Sexp; 
      if (smatch (&s, n, "shadowed"))
        break;
      s += n;
      n = snext (&s);
      if (!n)
        return GNUPG_Invalid_Sexp; 
      s +=n; /* skip value */
      if (*s != ')')
        return GNUPG_Invalid_Sexp; 
      depth--;
      s++;
    }
  /* found the shadowed list, s points to the protocol */
  n = snext (&s);
  if (!n) 
    return GNUPG_Invalid_Sexp; 
  if (smatch (&s, n, "t1-v1"))
    {
      if (*s != '(')
        return GNUPG_Invalid_Sexp;
      *shadow_info = s;
    }
  else
    return GNUPG_Unsupported_Protocol;
  return 0;
}

