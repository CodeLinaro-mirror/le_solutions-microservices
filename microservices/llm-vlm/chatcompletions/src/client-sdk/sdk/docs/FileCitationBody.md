# FileCitationBody

A citation to a file.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the file citation. Always &#x60;file_citation&#x60;. | [default to 'file_citation']
**file_id** | **str** | The ID of the file. | 
**index** | **int** | The index of the file in the list of files. | 

## Example

```python
from openapi_client.models.file_citation_body import FileCitationBody

# TODO update the JSON string below
json = "{}"
# create an instance of FileCitationBody from a JSON string
file_citation_body_instance = FileCitationBody.from_json(json)
# print the JSON string representation of the object
print(FileCitationBody.to_json())

# convert the object into a dict
file_citation_body_dict = file_citation_body_instance.to_dict()
# create an instance of FileCitationBody from a dict
file_citation_body_from_dict = FileCitationBody.from_dict(file_citation_body_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


